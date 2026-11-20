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
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_create_aggregate() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE AGGREGATE with SFUNC and STYPE",
            set_up_script: &[
                r#"CREATE FUNCTION agg_sum_step(state int4, val int4) RETURNS int4
					AS $$ SELECT state + val $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_sum_vals (pk int4 PRIMARY KEY, grp text, v int4);",
                "INSERT INTO agg_sum_vals VALUES (1, 'a', 10), (2, 'a', 20), (3, 'b', 5), (4, 'b', NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_sum (int4) (SFUNC = agg_sum_step, STYPE = int4, INITCOND = '0');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_sum(v) FROM agg_sum_vals WHERE grp = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("agg_sum", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grp, agg_sum(v) FROM agg_sum_vals GROUP BY grp ORDER BY grp;",
                    expected: Expected::Rows {
                        columns: &[Column("grp", TEXT), Column("agg_sum", INT4)],
                        rows: &[
                            &[T("a"), T("30")],
                            &[T("b"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_sum(v) FROM agg_sum_vals WHERE pk = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_sum", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aggkind, agginitval, aggfinalfn::text, aggcombinefn::text FROM pg_aggregate WHERE aggtransfn::text = 'agg_sum_step';",
                    expected: Expected::Rows {
                        columns: &[Column("aggkind", CHAR), Column("agginitval", TEXT), Column("aggfinalfn", TEXT), Column("aggcombinefn", TEXT)],
                        rows: &[
                            &[T("n"), T("0"), T("-"), T("-")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, prokind FROM pg_proc WHERE proname = 'agg_sum';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("prokind", CHAR)],
                        rows: &[
                            &[T("agg_sum"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE with STRICT transition function and no INITCOND",
            set_up_script: &[
                r#"CREATE FUNCTION agg_larger_step(state int4, val int4) RETURNS int4
					AS $$ SELECT CASE WHEN state > val THEN state ELSE val END $$ LANGUAGE SQL STRICT;"#,
                "CREATE TABLE agg_larger_vals (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO agg_larger_vals VALUES (1, 3), (2, NULL), (3, 8), (4, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_larger (int4) (SFUNC = agg_larger_step, STYPE = int4);",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_larger(v) FROM agg_larger_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_larger", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_larger(v) FROM agg_larger_vals WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_larger", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_larger(v) FROM agg_larger_vals WHERE pk = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_larger", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agginitval IS NULL FROM pg_aggregate WHERE aggtransfn::text = 'agg_larger_step';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
            name: "CREATE AGGREGATE with FINALFUNC",
            set_up_script: &[
                r#"CREATE FUNCTION agg_charcount_step(state int4, val text) RETURNS int4
					AS $$ SELECT state + length(val) $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION agg_charcount_final(state int4) RETURNS text
					AS $$ SELECT 'chars: ' || state $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_charcount_vals (pk int4 PRIMARY KEY, v text);",
                "INSERT INTO agg_charcount_vals VALUES (1, 'ab'), (2, 'cde'), (3, 'f');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE AGGREGATE agg_charcount (text) (SFUNC = agg_charcount_step, STYPE = int4,
						FINALFUNC = agg_charcount_final, INITCOND = '0');"#,
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_charcount(v) FROM agg_charcount_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_charcount", TEXT)],
                        rows: &[
                            &[T("chars: 6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_charcount(v) FROM agg_charcount_vals WHERE pk = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_charcount", TEXT)],
                        rows: &[
                            &[T("chars: 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aggfinalfn::text FROM pg_aggregate WHERE aggtransfn::text = 'agg_charcount_step';",
                    expected: Expected::Rows {
                        columns: &[Column("aggfinalfn", TEXT)],
                        rows: &[
                            &[T("agg_charcount_final")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE with COMBINEFUNC",
            set_up_script: &[
                r#"CREATE FUNCTION agg_combined_step(state int8, val int4) RETURNS int8
					AS $$ SELECT state + val $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION agg_combined_merge(s1 int8, s2 int8) RETURNS int8
					AS $$ SELECT s1 + s2 $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_combined_vals (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO agg_combined_vals VALUES (1, 10), (2, 20), (3, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE AGGREGATE agg_combined (int4) (SFUNC = agg_combined_step, STYPE = int8,
						COMBINEFUNC = agg_combined_merge, INITCOND = '0');"#,
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_combined(v) FROM agg_combined_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_combined", INT8)],
                        rows: &[
                            &[T("35")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aggcombinefn::text FROM pg_aggregate WHERE aggtransfn::text = 'agg_combined_step';",
                    expected: Expected::Rows {
                        columns: &[Column("aggcombinefn", TEXT)],
                        rows: &[
                            &[T("agg_combined_merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE with multiple arguments",
            set_up_script: &[
                r#"CREATE FUNCTION agg_wsum_step(state int8, val int4, weight int4) RETURNS int8
					AS $$ SELECT state + (val * weight) $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_wsum_vals (pk int4 PRIMARY KEY, v int4, w int4);",
                "INSERT INTO agg_wsum_vals VALUES (1, 10, 1), (2, 20, 2), (3, 30, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_wsum (int4, int4) (SFUNC = agg_wsum_step, STYPE = int8, INITCOND = '0');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_wsum(v, w) FROM agg_wsum_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_wsum", INT8)],
                        rows: &[
                            &[T("140")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE in a custom schema",
            set_up_script: &[
                "CREATE SCHEMA agg_nsp;",
                r#"CREATE FUNCTION agg_nsp_step(state int4, val int4) RETURNS int4
					AS $$ SELECT state + 1 $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_nsp_vals (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO agg_nsp_vals VALUES (1, 10), (2, 20), (3, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_nsp.agg_rows (int4) (SFUNC = agg_nsp_step, STYPE = int4, INITCOND = '0');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_nsp.agg_rows(v) FROM agg_nsp_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_rows", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.nspname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.proname = 'agg_rows';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("agg_nsp")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_rows(v) FROM agg_nsp_vals;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function agg_rows(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OR REPLACE AGGREGATE",
            set_up_script: &[
                r#"CREATE FUNCTION agg_replace_step(state int4, val int4) RETURNS int4
					AS $$ SELECT state + val $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_replace_vals (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO agg_replace_vals VALUES (1, 10), (2, 20);",
                "CREATE AGGREGATE agg_replace (int4) (SFUNC = agg_replace_step, STYPE = int4, INITCOND = '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT agg_replace(v) FROM agg_replace_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_replace", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE AGGREGATE agg_replace (int4) (SFUNC = agg_replace_step, STYPE = int4, INITCOND = '100');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_replace(v) FROM agg_replace_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_replace", INT4)],
                        rows: &[
                            &[T("130")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agginitval FROM pg_aggregate WHERE aggtransfn::text = 'agg_replace_step';",
                    expected: Expected::Rows {
                        columns: &[Column("agginitval", TEXT)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP AGGREGATE smoke test",
            set_up_script: &[
                r#"CREATE FUNCTION agg_drop_step(state int4, val int4) RETURNS int4
					AS $$ SELECT state + val $$ LANGUAGE SQL;"#,
                "CREATE TABLE agg_drop_vals (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO agg_drop_vals VALUES (1, 10), (2, 20);",
                "CREATE AGGREGATE agg_drop (int4) (SFUNC = agg_drop_step, STYPE = int4, INITCOND = '0');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT agg_drop(v) FROM agg_drop_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_drop", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP AGGREGATE agg_drop(int4);",
                    expected: Expected::Tag("DROP AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_drop(v) FROM agg_drop_vals;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function agg_drop(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM pg_proc WHERE proname = 'agg_drop');",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_drop (int4) (SFUNC = agg_drop_step, STYPE = int4, INITCOND = '0');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT agg_drop(v) FROM agg_drop_vals;",
                    expected: Expected::Rows {
                        columns: &[Column("agg_drop", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE validation",
            set_up_script: &[
                r#"CREATE FUNCTION agg_valid_step(state int4, val int4) RETURNS int4
					AS $$ SELECT state + val $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION agg_one_arg(state int4) RETURNS int4
					AS $$ SELECT state $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION agg_wrong_ret(state int4, val int4) RETURNS text
					AS $$ SELECT 'x' $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION agg_taken(a int4) RETURNS int4
					AS $$ SELECT a $$ LANGUAGE SQL;"#,
                "CREATE AGGREGATE agg_valid (int4) (SFUNC = agg_valid_step, STYPE = int4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_bad1 (int4) (SFUNC = agg_missing_step, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function agg_missing_step(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_bad2 (int4) (SFUNC = agg_one_arg, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function agg_one_arg(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_bad3 (int4) (SFUNC = agg_wrong_ret, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "return type of transition function agg_wrong_ret is not integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_bad4 (int4) (SFUNC = agg_valid_step, STYPE = int4, FINALFUNC = agg_missing_final);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function agg_missing_final(integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_valid (int4) (SFUNC = agg_valid_step, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42723", message: r#"function "agg_valid" already exists with same argument types"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_taken (int4) (SFUNC = agg_valid_step, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42723", message: r#"function "agg_taken" already exists with same argument types"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_bad5 (OUT x int4) (SFUNC = agg_valid_step, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "aggregates cannot have output arguments", position: 28, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE AGGREGATE options and DROP AGGREGATE errors",
            set_up_script: &[
                r#"CREATE FUNCTION agg_opt_step(state int8, val int4) RETURNS int8
					AS $$ SELECT state + val $$ LANGUAGE SQL STRICT;"#,
                r#"CREATE FUNCTION agg_opt_taken(a int4) RETURNS int4
					AS $$ SELECT a $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_opt (int4) (SFUNC = agg_opt_step, STYPE = int8);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "must not omit initial value when transition function is strict and transition type is not compatible with input type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_opt (int4) (SFUNC = agg_opt_step);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "aggregate stype must be specified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_opt (int4) (STYPE = int8);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "aggregate sfunc must be specified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE AGGREGATE agg_opt_taken (int4) (SFUNC = agg_opt_step, STYPE = int8, INITCOND = 0);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot change routine kind", detail: r#""agg_opt_taken" is a function."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE agg_opt (int4) (SFUNC = agg_opt_step, STYPE = int8, BOGUS = 1, INITCOND = 0);",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42601", message: r#"aggregate attribute "bogus" not recognized"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, prokind, pronargs, prorettype, proargtypes::text, prosrc, provolatile FROM pg_proc WHERE proname = 'agg_opt';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("prokind", CHAR), Column("pronargs", INT2), Column("prorettype", OID), Column("proargtypes", TEXT), Column("prosrc", TEXT), Column("provolatile", CHAR)],
                        rows: &[
                            &[T("agg_opt"), T("a"), T("1"), T("20"), T("23"), T("aggregate_dummy"), T("i")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP AGGREGATE agg_opt(int8);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "aggregate agg_opt(bigint) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP AGGREGATE IF EXISTS agg_opt(int8);",
                    expected: Expected::Tag("DROP AGGREGATE"),
                    notices: &[Diagnostic { code: "00000", message: "aggregate agg_opt(int8) does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP AGGREGATE agg_opt(int4);",
                    expected: Expected::Tag("DROP AGGREGATE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_aggregate_support_functions_through_fmgr() {
    run_scripts(&[
        ScriptTest {
            name: "SECURITY DEFINER and SET clause support functions called through fmgr",
            set_up_script: &[
                "CREATE ROLE fm_owner;",
                "CREATE ROLE fm_caller;",
                "CREATE TABLE fm_t (i int4, v text);",
                "INSERT INTO fm_t VALUES (1, 'a'), (2, 'b'), (3, 'a'), (4, NULL);",
                "GRANT SELECT ON fm_t TO fm_caller;",
                "CREATE FUNCTION fm_sql_sfunc(text, text) RETURNS text LANGUAGE sql SECURITY DEFINER AS $$ SELECT coalesce($1, '') || coalesce($2, '-') || ':' || current_user || ' ' $$;",
                "ALTER FUNCTION fm_sql_sfunc(text, text) OWNER TO fm_owner;",
                "CREATE FUNCTION fm_pl_sfunc(text, text) RETURNS text LANGUAGE plpgsql SECURITY DEFINER AS $$ BEGIN RETURN coalesce($1, '') || coalesce($2, '-') || ':' || current_user || ' '; END $$;",
                "ALTER FUNCTION fm_pl_sfunc(text, text) OWNER TO fm_owner;",
                "CREATE FUNCTION fm_pl_final(text) RETURNS text LANGUAGE plpgsql SECURITY DEFINER AS $$ BEGIN RETURN $1 || 'final:' || current_user; END $$;",
                "ALTER FUNCTION fm_pl_final(text) OWNER TO fm_owner;",
                "CREATE AGGREGATE fm_sql_agg(text) (SFUNC = fm_sql_sfunc, STYPE = text);",
                "CREATE AGGREGATE fm_pl_agg(text) (SFUNC = fm_pl_sfunc, STYPE = text, FINALFUNC = fm_pl_final);",
                "CREATE FUNCTION fm_set_sfunc(text, text) RETURNS text LANGUAGE sql SET work_mem = '1234kB' SET search_path = pg_catalog AS $$ SELECT coalesce($1, '') || current_setting('work_mem') || '/' || current_setting('search_path') || ' ' $$;",
                "CREATE AGGREGATE fm_set_agg(text) (SFUNC = fm_set_sfunc, STYPE = text);",
                "CREATE FUNCTION fm_err_sfunc(int4, int4) RETURNS int4 LANGUAGE plpgsql SET work_mem = '777kB' AS $$ BEGIN IF $2 = 3 THEN RAISE EXCEPTION 'boom at %', current_setting('work_mem'); END IF; RETURN coalesce($1, 0) + $2; END $$;",
                "CREATE AGGREGATE fm_err_agg(int4) (SFUNC = fm_err_sfunc, STYPE = int4);",
                "CREATE FUNCTION fm_eq(int4, text) RETURNS bool LANGUAGE plpgsql SECURITY DEFINER SET work_mem = '4321kB' AS $$ BEGIN RAISE NOTICE 'fm_eq % % as % with %', $1, $2, current_user, current_setting('work_mem'); RETURN $1::text = $2; END $$;",
                "ALTER FUNCTION fm_eq(int4, text) OWNER TO fm_owner;",
                "CREATE OPERATOR = (LEFTARG = int4, RIGHTARG = text, FUNCTION = fm_eq);",
                "CREATE FUNCTION fm_show(text) RETURNS text LANGUAGE internal STABLE SET work_mem = '999kB' AS 'show_config_by_name';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE fm_caller;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_sql_agg(v ORDER BY i) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_sql_agg", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner a:fm_owner -:fm_owner ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_pl_agg(v ORDER BY i) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_pl_agg", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner a:fm_owner -:fm_owner final:fm_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_sql_agg(DISTINCT v) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_sql_agg", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner -:fm_owner ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_pl_agg(DISTINCT v) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_pl_agg", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner -:fm_owner final:fm_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, current_setting('work_mem'), current_setting('search_path');",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("current_setting", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("fm_caller"), T("4MB"), T("\"$user\", public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_set_agg(v ORDER BY i) FROM fm_t WHERE v IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_set_agg", TEXT)],
                        rows: &[
                            &[T("1234kB/pg_catalog 1234kB/pg_catalog 1234kB/pg_catalog ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('work_mem'), current_setting('search_path');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("4MB"), T("\"$user\", public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_show('work_mem'), current_setting('work_mem');",
                    expected: Expected::Rows {
                        columns: &[Column("fm_show", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("999kB"), T("4MB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_err_agg(i ORDER BY i) FROM fm_t;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "boom at 777kB", context: Some("PL/pgSQL function fm_err_sfunc(integer,integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('work_mem');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("4MB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_err_agg(i ORDER BY i) FROM fm_t WHERE i <> 3;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_err_agg", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULLIF(fm_sql_agg(v ORDER BY i), 'x') FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("nullif", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner a:fm_owner -:fm_owner ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_pl_agg(v ORDER BY i) = ANY (ARRAY['x', 'y']) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULLIF(1, '1'::text), NULLIF(2, '1'::text);",
                    notices: &[Diagnostic { code: "00000", message: "fm_eq 1 1 as fm_owner with 4321kB", context: Some("PL/pgSQL function fm_eq(integer,text) line 1 at RAISE"), ..N }, Diagnostic { code: "00000", message: "fm_eq 2 1 as fm_owner with 4321kB", context: Some("PL/pgSQL function fm_eq(integer,text) line 1 at RAISE"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("nullif", INT4), Column("nullif", INT4)],
                        rows: &[
                            &[Null, T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2 = ANY (ARRAY['1', '2', '3']::text[]);",
                    notices: &[Diagnostic { code: "00000", message: "fm_eq 2 1 as fm_owner with 4321kB", context: Some("PL/pgSQL function fm_eq(integer,text) line 1 at RAISE"), ..N }, Diagnostic { code: "00000", message: "fm_eq 2 2 as fm_owner with 4321kB", context: Some("PL/pgSQL function fm_eq(integer,text) line 1 at RAISE"), ..N }],
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
                    query: "SELECT i, fm_sql_agg(v) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM fm_t ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("fm_sql_agg", TEXT)],
                        rows: &[
                            &[T("1"), T("a:fm_owner ")],
                            &[T("2"), T("a:fm_owner b:fm_owner ")],
                            &[T("3"), T("b:fm_owner a:fm_owner ")],
                            &[T("4"), T("a:fm_owner -:fm_owner ")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i, fm_set_agg(v) OVER (ORDER BY i) FROM fm_t WHERE v IS NOT NULL ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("fm_set_agg", TEXT)],
                        rows: &[
                            &[T("1"), T("1234kB/pg_catalog ")],
                            &[T("2"), T("1234kB/pg_catalog 1234kB/pg_catalog ")],
                            &[T("3"), T("1234kB/pg_catalog 1234kB/pg_catalog 1234kB/pg_catalog ")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('work_mem');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("4MB")],
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
                    query: "REVOKE EXECUTE ON FUNCTION fm_pl_sfunc(text, text) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE EXECUTE ON FUNCTION fm_eq(int4, text) FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE fm_caller;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_pl_agg(v ORDER BY i) FROM fm_t;",
                    expected: Expected::Rows {
                        columns: &[Column("fm_pl_agg", TEXT)],
                        rows: &[
                            &[T("a:fm_owner b:fm_owner a:fm_owner -:fm_owner final:fm_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULLIF(1, '1'::text);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function fm_eq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2 = ANY (ARRAY['1', '2', '3']::text[]);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function fm_eq", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER AGGREGATE fm_pl_agg(text) OWNER TO fm_caller;",
                    expected: Expected::Tag("ALTER AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE fm_caller;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fm_pl_agg(v) FROM fm_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function fm_pl_sfunc", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i, fm_pl_agg(v) OVER (ORDER BY i) FROM fm_t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for function fm_pl_sfunc", ..E }),
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
fn test_aggregate_initial_values_read_in_the_session() {
    run_scripts(&[
        ScriptTest {
            name: "an aggregate's initial value is read in the session, here a domain whose CHECK calls a SQL function",
            set_up_script: &[
                "CREATE FUNCTION is_small(i INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT i < 1000 $$;",
                "CREATE DOMAIN small AS INT CHECK (is_small(VALUE));",
                "CREATE FUNCTION add_small(s small, i INT) RETURNS small IMMUTABLE LANGUAGE sql AS $$ SELECT (s + i)::small $$;",
                "CREATE AGGREGATE small_sum(INT) (sfunc = add_small, stype = small, initcond = '100');",
                "CREATE TABLE si (g INT, v INT);",
                "INSERT INTO si VALUES (1, 1), (1, 2), (2, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT g, small_sum(v) FROM si GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("small_sum", INT4)],
                        rows: &[
                            &[T("1"), T("103")],
                            &[T("2"), T("110")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT small_sum(v) FROM si WHERE false;",
                    expected: Expected::Rows {
                        columns: &[Column("small_sum", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, v, small_sum(v) OVER (PARTITION BY g ORDER BY v) FROM si ORDER BY g, v;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("v", INT4), Column("small_sum", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("101")],
                            &[T("1"), T("2"), T("103")],
                            &[T("2"), T("10"), T("110")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE bad_sum(INT) (sfunc = add_small, stype = small, initcond = '5000');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain small violates check constraint \"small_check\"", schema: "public", data_type: "small", constraint: "small_check", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_replace_aggregate_keeps_one_row() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE OR REPLACE AGGREGATE replaces its pg_aggregate row and keeps its kind",
            set_up_script: &[
                "CREATE FUNCTION sstep(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE AS 'SELECT $1 + $2';",
                "CREATE FUNCTION sstep3(int4, int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE AS 'SELECT $1 + $2 + $3';",
                "CREATE AGGREGATE ragg (int4) (SFUNC = sstep, STYPE = int4);",
                "CREATE AGGREGATE ragg2 (int4, int4) (SFUNC = sstep3, STYPE = int4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE AGGREGATE ragg (int4) (SFUNC = sstep, STYPE = int4, INITCOND = '5');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ragg(x) FROM (VALUES (1), (2)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("ragg", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_aggregate WHERE aggfnoid = 'ragg(int4)'::regprocedure;",
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
                    query: "CREATE OR REPLACE AGGREGATE ragg2 (int4 ORDER BY int4) (SFUNC = sstep, STYPE = int4);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot change routine kind", detail: r#""ragg2" is an ordinary aggregate function."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_polymorphic_support_functions_resolve_the_state_type() {
    run_scripts(&[
        ScriptTest {
            name: "SQL and PL/pgSQL transition and final functions returning anyarray or anyelement run with the aggregate's resolved types",
            set_up_script: &[
                "CREATE FUNCTION first_el_transfn(anyarray, anyelement) RETURNS anyarray AS 'SELECT $1 || $2' LANGUAGE sql IMMUTABLE;",
                "CREATE FUNCTION first_el(anyarray) RETURNS anyelement AS 'SELECT $1[1]' LANGUAGE sql STRICT IMMUTABLE;",
                "CREATE AGGREGATE first_el_agg_any(anyelement) (SFUNC = first_el_transfn, STYPE = anyarray, FINALFUNC = first_el);",
                "CREATE AGGREGATE first_el_agg_f8(float8) (SFUNC = array_append, STYPE = float8[], FINALFUNC = first_el);",
                "CREATE FUNCTION add_group(grp anyarray, ad anyelement, size INTEGER) RETURNS anyarray AS $$ BEGIN IF grp IS NULL THEN RETURN ARRAY[ad]; END IF; IF array_upper(grp, 1) < size THEN RETURN grp || ad; END IF; RETURN grp; END; $$ LANGUAGE plpgsql IMMUTABLE;",
                "CREATE AGGREGATE build_group(anyelement, INTEGER) (SFUNC = add_group, STYPE = anyarray);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT first_el_agg_any(x) FROM generate_series(1, 10) x;",
                    expected: Expected::Rows {
                        columns: &[Column("first_el_agg_any", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_el_agg_f8(x::float8) FROM generate_series(1, 10) x;",
                    expected: Expected::Rows {
                        columns: &[Column("first_el_agg_f8", FLOAT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_el_agg_any(x) OVER (ORDER BY x) FROM generate_series(1, 3) x;",
                    expected: Expected::Rows {
                        columns: &[Column("first_el_agg_any", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT build_group(x, 3) FROM generate_series(1, 5) x;",
                    expected: Expected::Rows {
                        columns: &[Column("build_group", INT4_ARRAY)],
                        rows: &[
                            &[T("{1,2,3}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT build_group(x::text, 2) FROM generate_series(1, 5) x;",
                    expected: Expected::Rows {
                        columns: &[Column("build_group", TEXT_ARRAY)],
                        rows: &[
                            &[T("{1,2}")],
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
fn test_not_owner_messages_of_aggregates_and_procedures() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER of an aggregate or procedure by a non-owner names it a function, DROP and COMMENT by its kind",
            set_up_script: &[
                "CREATE ROLE aown_u",
                "CREATE SCHEMA aown_s",
                "GRANT ALL ON SCHEMA aown_s TO aown_u",
                "CREATE AGGREGATE aown_s.agg(int) (sfunc = int4pl, stype = int)",
                "CREATE PROCEDURE aown_s.pr() LANGUAGE sql AS 'SELECT 1'",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE aown_u;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER AGGREGATE aown_s.agg(int) RENAME TO agg2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function agg", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER AGGREGATE aown_s.agg(int) OWNER TO aown_u;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function agg", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PROCEDURE aown_s.pr() RENAME TO pr2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function pr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROUTINE aown_s.pr() OWNER TO aown_u;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function pr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP AGGREGATE aown_s.agg(int);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of aggregate aown_s.agg", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE aown_s.pr();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of procedure aown_s.pr", ..E }),
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
