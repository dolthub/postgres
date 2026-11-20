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
fn test_error_context() {
    run_scripts(&[
        ScriptTest {
            name: "context of calls through PL/pgSQL, SQL functions, and SPI",
            set_up_script: &[
                "CREATE FUNCTION ctx_bottom(x INT) RETURNS INT LANGUAGE plpgsql AS E'BEGIN\\n  RAISE NOTICE ''bottom %'', x;\\n  IF x < 0 THEN\\n    RAISE EXCEPTION ''negative %'', x;\\n  END IF;\\n  RETURN 10 / x;\\nEND';",
                "CREATE FUNCTION ctx_middle(x INT) RETURNS INT LANGUAGE sql AS 'SELECT 1; SELECT ctx_bottom(x) + 1';",
                "CREATE FUNCTION ctx_top(x INT) RETURNS INT LANGUAGE plpgsql AS E'DECLARE\\n  r INT;\\nBEGIN\\n  r := ctx_middle(x);\\n  RETURN r;\\nEND';",
                "CREATE FUNCTION ctx_catch(x INT) RETURNS TEXT LANGUAGE plpgsql AS E'DECLARE\\n  c TEXT;\\nBEGIN\\n  RETURN ctx_top(x)::TEXT;\\nEXCEPTION WHEN OTHERS THEN\\n  GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT;\\n  RETURN c;\\nEND';",
                "CREATE FUNCTION ctx_catch_direct(x INT) RETURNS TEXT LANGUAGE plpgsql AS E'DECLARE\\n  c TEXT;\\n  r INT;\\nBEGIN\\n  BEGIN\\n    r := 10 / x;\\n  EXCEPTION WHEN division_by_zero THEN\\n    GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT;\\n    RETURN c;\\n  END;\\n  RETURN r::TEXT;\\nEND';",
                "CREATE FUNCTION ctx_catch_caller(x INT) RETURNS TEXT LANGUAGE sql AS 'SELECT 1; SELECT ctx_catch_direct(x)';",
                "CREATE FUNCTION ctx_reraise(x INT) RETURNS INT LANGUAGE plpgsql AS E'BEGIN\\n  RETURN ctx_top(x);\\nEXCEPTION WHEN division_by_zero THEN\\n  RAISE NOTICE ''caught'';\\n  RAISE;\\nEND';",
                "CREATE FUNCTION ctx_outer(x INT) RETURNS INT LANGUAGE sql AS 'SELECT 0; SELECT ctx_reraise(x)';",
                "CREATE FUNCTION ctx_stack() RETURNS TEXT LANGUAGE plpgsql AS E'DECLARE\\n  c TEXT;\\nBEGIN\\n  GET DIAGNOSTICS c = PG_CONTEXT;\\n  RETURN c;\\nEND';",
                "CREATE FUNCTION ctx_stack_caller() RETURNS TEXT LANGUAGE plpgsql AS E'BEGIN\\n  RETURN ctx_stack();\\nEND';",
                "CREATE FUNCTION ctx_stack_sql() RETURNS TEXT LANGUAGE sql AS 'SELECT ''x''; SELECT ctx_stack_caller()';",
                "CREATE FUNCTION ctx_ddl() RETURNS VOID LANGUAGE plpgsql AS E'BEGIN\\n  CREATE TABLE IF NOT EXISTS ctx_t (a INT);\\nEND';",
                "CREATE FUNCTION ctx_ddl_caller() RETURNS VOID LANGUAGE plpgsql AS E'BEGIN\\n  PERFORM ctx_ddl();\\nEND';",
                "CREATE FUNCTION ctx_dynamic(x INT) RETURNS INT LANGUAGE plpgsql AS E'DECLARE\\n  r INT;\\nBEGIN\\n  EXECUTE ''SELECT 10 / $1'' INTO r USING x;\\n  RETURN r;\\nEND';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ctx_top(5);",
                    notices: &[Diagnostic { code: "00000", message: "bottom 5", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("ctx_top", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_top(0);",
                    notices: &[Diagnostic { code: "00000", message: "bottom 0", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("PL/pgSQL function ctx_bottom(integer) line 6 at RETURN\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_top(-1);",
                    notices: &[Diagnostic { code: "00000", message: "bottom -1", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "negative -1", context: Some("PL/pgSQL function ctx_bottom(integer) line 4 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(ctx_catch(0), E'\\n', ' / ');",
                    notices: &[Diagnostic { code: "00000", message: "bottom 0", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_catch(integer) line 4 at RETURN"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_bottom(integer) line 6 at RETURN / SQL function \"ctx_middle\" statement 2 / PL/pgSQL function ctx_top(integer) line 4 at assignment / PL/pgSQL function ctx_catch(integer) line 4 at RETURN")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(ctx_catch(-1), E'\\n', ' / ');",
                    notices: &[Diagnostic { code: "00000", message: "bottom -1", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_catch(integer) line 4 at RETURN"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_bottom(integer) line 4 at RAISE / SQL function \"ctx_middle\" statement 2 / PL/pgSQL function ctx_top(integer) line 4 at assignment / PL/pgSQL function ctx_catch(integer) line 4 at RETURN")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_catch_direct(0);",
                    expected: Expected::Rows {
                        columns: &[Column("ctx_catch_direct", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_catch_direct(integer) line 6 at assignment")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(ctx_catch_caller(0), E'\\n', ' / ');",
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_catch_direct(integer) line 6 at assignment / SQL function \"ctx_catch_caller\" statement 2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_reraise(0);",
                    notices: &[Diagnostic { code: "00000", message: "bottom 0", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_reraise(integer) line 2 at RETURN"), ..N }, Diagnostic { code: "00000", message: "caught", context: Some("PL/pgSQL function ctx_reraise(integer) line 4 at RAISE"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("PL/pgSQL function ctx_bottom(integer) line 6 at RETURN\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_reraise(integer) line 2 at RETURN"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_outer(0);",
                    notices: &[Diagnostic { code: "00000", message: "bottom 0", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_reraise(integer) line 2 at RETURN\nSQL function \"ctx_outer\" statement 2"), ..N }, Diagnostic { code: "00000", message: "caught", context: Some("PL/pgSQL function ctx_reraise(integer) line 4 at RAISE\nSQL function \"ctx_outer\" statement 2"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("PL/pgSQL function ctx_bottom(integer) line 6 at RETURN\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nPL/pgSQL function ctx_reraise(integer) line 2 at RETURN\nSQL function \"ctx_outer\" statement 2"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_stack();",
                    expected: Expected::Rows {
                        columns: &[Column("ctx_stack", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_stack() line 4 at GET DIAGNOSTICS")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(ctx_stack_caller(), E'\\n', ' / ');",
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_stack() line 4 at GET DIAGNOSTICS / PL/pgSQL function ctx_stack_caller() line 2 at RETURN")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(ctx_stack_sql(), E'\\n', ' / ');",
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL function ctx_stack() line 4 at GET DIAGNOSTICS / PL/pgSQL function ctx_stack_caller() line 2 at RETURN / SQL function \"ctx_stack_sql\" statement 2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_ddl();",
                    expected: Expected::Rows {
                        columns: &[Column("ctx_ddl", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_ddl_caller();",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"ctx_t\" already exists, skipping", context: Some("SQL statement \"CREATE TABLE IF NOT EXISTS ctx_t (a INT)\"\nPL/pgSQL function ctx_ddl() line 2 at SQL statement\nSQL statement \"SELECT ctx_ddl()\"\nPL/pgSQL function ctx_ddl_caller() line 2 at PERFORM"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("ctx_ddl_caller", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx_dynamic(0);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("SQL statement \"SELECT 10 / $1\"\nPL/pgSQL function ctx_dynamic(integer) line 4 at EXECUTE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO E'BEGIN\\n  PERFORM ctx_top(0);\\nEND';",
                    notices: &[Diagnostic { code: "00000", message: "bottom 0", context: Some("PL/pgSQL function ctx_bottom(integer) line 2 at RAISE\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nSQL statement \"SELECT ctx_top(0)\"\nPL/pgSQL function inline_code_block line 2 at PERFORM"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("PL/pgSQL function ctx_bottom(integer) line 6 at RETURN\nSQL function \"ctx_middle\" statement 2\nPL/pgSQL function ctx_top(integer) line 4 at assignment\nSQL statement \"SELECT ctx_top(0)\"\nPL/pgSQL function inline_code_block line 2 at PERFORM"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO E'DECLARE\\n  c TEXT;\\nBEGIN\\n  GET DIAGNOSTICS c = PG_CONTEXT;\\n  RAISE NOTICE ''%'', c;\\nEND';",
                    notices: &[Diagnostic { code: "00000", message: "PL/pgSQL function inline_code_block line 4 at GET DIAGNOSTICS", context: Some("PL/pgSQL function inline_code_block line 5 at RAISE"), ..N }],
                    expected: Expected::Tag("DO"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "context of Bind's parameters and COPY FROM",
            set_up_script: &[
                "CREATE TABLE ctx_copy (a INT CHECK (a <> 5));",
                "CREATE FUNCTION ctx_trg() RETURNS TRIGGER LANGUAGE plpgsql AS E'BEGIN\\n  RAISE NOTICE ''row %'', NEW.a;\\n  IF NEW.a < 0 THEN\\n    RAISE EXCEPTION ''negative row %'', NEW.a;\\n  END IF;\\n  RETURN NEW;\\nEND';",
                "CREATE TRIGGER ctx_trg BEFORE INSERT ON ctx_copy FOR EACH ROW EXECUTE FUNCTION ctx_trg();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT $1::INT;",
                    bind_vars: &[BindVar::Str("abc")],
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"abc\"", context: Some("unnamed portal parameter $1 = '...'"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET log_parameter_max_length_on_error = -1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $1::INT;",
                    bind_vars: &[BindVar::Str("abc")],
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"abc\"", context: Some("unnamed portal parameter $1 = 'abc'"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 10 / $1::INT;",
                    bind_vars: &[BindVar::Str("0")],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("unnamed portal with parameters: $1 = '0'"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET log_parameter_max_length_on_error = 2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 10 / $1::INT, $2::TEXT;",
                    bind_vars: &[BindVar::Str("0"), BindVar::Str("abcdef")],
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", context: Some("unnamed portal with parameters: $1 = '0', $2 = 'ab...'"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY ctx_copy FROM STDIN;",
                    copy_from_stdin_file: "error-context-copy.sql",
                    notices: &[
                        Diagnostic { code: "00000", message: "row 8", context: Some("PL/pgSQL function ctx_trg() line 2 at RAISE\nCOPY ctx_copy, line 1: \"8\""), ..N },
                        Diagnostic { code: "00000", message: "row -2", context: Some("PL/pgSQL function ctx_trg() line 2 at RAISE\nCOPY ctx_copy, line 2: \"-2\""), ..N },
                    ],
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "negative row -2", context: Some("PL/pgSQL function ctx_trg() line 4 at RAISE\nCOPY ctx_copy, line 2: \"-2\""), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY ctx_copy FROM STDIN;",
                    copy_from_stdin_file: "error-context-copy-check.sql",
                    notices: &[
                        Diagnostic { code: "00000", message: "row 8", context: Some("PL/pgSQL function ctx_trg() line 2 at RAISE\nCOPY ctx_copy, line 1: \"8\""), ..N },
                        Diagnostic { code: "00000", message: "row 5", context: Some("PL/pgSQL function ctx_trg() line 2 at RAISE\nCOPY ctx_copy, line 2: \"5\""), ..N },
                    ],
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"ctx_copy\" violates check constraint \"ctx_copy_a_check\"", detail: "Failing row contains (5).", schema: "public", table: "ctx_copy", constraint: "ctx_copy_a_check", context: Some("COPY ctx_copy, line 2: \"5\""), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY ctx_copy FROM STDIN;",
                    copy_from_stdin_file: "error-context-copy-input.sql",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"x\"", context: Some("COPY ctx_copy, line 1, column a: \"x\""), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "context of a notice a domain's check raises during Bind",
            set_up_script: &[
                "CREATE FUNCTION ctx_check(x INT) RETURNS BOOL LANGUAGE plpgsql AS E'BEGIN\\n  RAISE NOTICE ''checking %'', x;\\n  RETURN x > 0;\\nEND';",
                "CREATE DOMAIN ctx_pos AS INT CHECK (ctx_check(VALUE));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT $1::ctx_pos;",
                    bind_vars: &[BindVar::Str("5")],
                    notices: &[Diagnostic { code: "00000", message: "checking 5", context: Some("PL/pgSQL function ctx_check(integer) line 2 at RAISE\nunnamed portal parameter $1 = '...'"), ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("ctx_pos", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $1::ctx_pos;",
                    bind_vars: &[BindVar::Str("-1")],
                    notices: &[Diagnostic { code: "00000", message: "checking -1", context: Some("PL/pgSQL function ctx_check(integer) line 2 at RAISE\nunnamed portal parameter $1 = '...'"), ..N }],
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain ctx_pos violates check constraint \"ctx_pos_check\"", schema: "public", data_type: "ctx_pos", constraint: "ctx_pos_check", context: Some("unnamed portal parameter $1 = '...'"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ctx_copy_dom (a ctx_pos);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY ctx_copy_dom FROM STDIN;",
                    copy_from_stdin_file: "error-context-copy.sql",
                    notices: &[
                        Diagnostic { code: "00000", message: "checking 8", context: Some("PL/pgSQL function ctx_check(integer) line 2 at RAISE\nCOPY ctx_copy_dom, line 1, column a: \"8\""), ..N },
                        Diagnostic { code: "00000", message: "checking -2", context: Some("PL/pgSQL function ctx_check(integer) line 2 at RAISE\nCOPY ctx_copy_dom, line 2, column a: \"-2\""), ..N },
                    ],
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain ctx_pos violates check constraint \"ctx_pos_check\"", schema: "public", data_type: "ctx_pos", constraint: "ctx_pos_check", context: Some("COPY ctx_copy_dom, line 2, column a: \"-2\""), ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
