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
fn test_create_functions_language_sql() {
    run_scripts(&[
        ScriptTest {
            name: "unnamed parameter",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION alt_func1(int) RETURNS int LANGUAGE sql AS 'SELECT $1 + 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT alt_func1(3);",
                    expected: Expected::Rows {
                        columns: &[Column("alt_func1", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default on input parameters",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION alt_func1(int = 2, int) RETURNS int LANGUAGE sql AS 'SELECT $1 + $2';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "input parameters after one with a default value must also have defaults", position: 36, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION alt_func1(int = 2, int = 3) RETURNS int LANGUAGE sql AS 'SELECT $1 + $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT alt_func1();",
                    expected: Expected::Rows {
                        columns: &[Column("alt_func1", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT alt_func1(12);",
                    expected: Expected::Rows {
                        columns: &[Column("alt_func1", INT4)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "named parameter",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION alt_func1(x int) RETURNS int LANGUAGE sql AS 'SELECT x + 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT alt_func1(3);",
                    expected: Expected::Rows {
                        columns: &[Column("alt_func1", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sub_numbers(x int, y int) RETURNS int LANGUAGE sql AS 'SELECT y - x';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sub_numbers(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("sub_numbers", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nil default on input parameters",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"create function dfunc(a varchar = 'def a', out _a varchar, c numeric = NULL, out _c numeric)
							returns record as $$ select $1, $2; $$ language sql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select dfunc('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("dfunc", RECORD)],
                        rows: &[
                            &[T("(Hello,)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from dfunc('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("_a", VARCHAR), Column("_c", NUMERIC)],
                        rows: &[
                            &[T("Hello"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unknown functions",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION get_grade_description(score INT)
							RETURNS TEXT
							LANGUAGE SQL
							AS $$
								SELECT
									CASE
										WHEN score >= 90 THEN 'Excellent'
										WHEN score >= 75 THEN 'Good'
										WHEN score >= 50 THEN 'Average'
									ELSE 'Fail'
									END;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT get_grade_description(92);",
                    expected: Expected::Rows {
                        columns: &[Column("get_grade_description", TEXT)],
                        rows: &[
                            &[T("Excellent")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT get_grade_description(65);",
                    expected: Expected::Rows {
                        columns: &[Column("get_grade_description", TEXT)],
                        rows: &[
                            &[T("Average")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested functions",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION calculate_double_sum(x INT, y INT)
							RETURNS INT
							LANGUAGE SQL
							AS $$
								SELECT add_numbers(x, y) * 2;
							$$;"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function add_numbers(integer, integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 119, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION add_numbers(int, int) RETURNS int LANGUAGE sql AS 'SELECT $1 + $2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION calculate_double_sum(x INT, y INT)
							RETURNS INT
							LANGUAGE SQL
							AS $$
								SELECT add_numbers(x, y) * 2;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT calculate_double_sum(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("calculate_double_sum", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function returning multiple rows",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION gen(a int) RETURNS SETOF INT LANGUAGE SQL AS $$ SELECT generate_series(1, a) $$ STABLE;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM gen(3);",
                    expected: Expected::Rows {
                        columns: &[Column("gen", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function with create or replace view",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.sp_build_view_bathymetry_layer() RETURNS void
							LANGUAGE sql
							AS $$
								CREATE OR REPLACE VIEW public.view_bathymetry_layer AS
								SELECT 1;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp_build_view_bathymetry_layer()",
                    expected: Expected::Rows {
                        columns: &[Column("sp_build_view_bathymetry_layer", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * from view_bathymetry_layer",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp_build_view_bathymetry_layer()",
                    expected: Expected::Rows {
                        columns: &[Column("sp_build_view_bathymetry_layer", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * from view_bathymetry_layer",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function with update ... returning",
            set_up_script: &[
                r#"CREATE TYPE public.tax_job_state AS ENUM (
					'sched',
					'busy',
					'final',
					'help'
				);"#,
                r#"CREATE TABLE public.tax_job (
					id bigint NOT NULL,
					state public.tax_job_state NOT NULL,
					created timestamp NOT NULL,
					modified timestamp NOT NULL,
					scheduled timestamp,
					worker text,
					processor text,
					ext_id text,
					data jsonb,
					gross integer,
					notes text[],
					ops jsonb,
					CONSTRAINT tax_job_check CHECK ((NOT ((state = 'sched'::public.tax_job_state) AND (scheduled IS NULL)))),
					CONSTRAINT tax_job_check1 CHECK ((NOT ((state = 'busy'::public.tax_job_state) AND (worker IS NULL))))
				);"#,
                "INSERT INTO tax_job (id, state, created, modified, scheduled, worker, processor, ext_id, data) VALUES (1, 'sched', '2025-05-05 05:05:05', '2025-05-05 05:05:05', '2025-05-05 05:05:05', 'worker', 'processor', 'ext_id', NULL)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.tax_job_take(arg_worker text) RETURNS SETOF public.tax_job
								LANGUAGE sql
								AS '
								UPDATE
									tax_job
								SET
									state = ''busy'',
									worker = arg_worker
								WHERE
									state = ''sched''
									AND scheduled <= CURRENT_TIMESTAMP
								RETURNING
									*;
							';"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.tax_job_take('worker')",
                    expected: Expected::Rows {
                        columns: &[Column("tax_job_take", USER_DEFINED)],
                        rows: &[
                            &[T(r#"(1,busy,"2025-05-05 05:05:05","2025-05-05 05:05:05","2025-05-05 05:05:05",worker,processor,ext_id,,,,)"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tax_job (id, state, created, modified, scheduled, worker, processor, ext_id, data) VALUES (2, 'sched', '2025-05-05 05:05:06', '2025-05-05 05:05:06', '2025-05-05 05:05:06', 'worker', 'processor', 'ext_id', NULL), (3, 'sched', '2025-05-05 05:05:07', '2025-05-05 05:05:07', '2025-05-05 05:05:07', 'worker', 'processor', 'ext_id', NULL)",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.tax_job_take('worker')",
                    expected: Expected::Rows {
                        columns: &[Column("tax_job_take", USER_DEFINED)],
                        rows: &[
                            &[T(r#"(2,busy,"2025-05-05 05:05:06","2025-05-05 05:05:06","2025-05-05 05:05:06",worker,processor,ext_id,,,,)"#)],
                            &[T(r#"(3,busy,"2025-05-05 05:05:07","2025-05-05 05:05:07","2025-05-05 05:05:07",worker,processor,ext_id,,,,)"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function with delete",
            set_up_script: &[
                "CREATE TABLE test (id bigint NOT NULL, state text NOT NULL);",
                "INSERT INTO test VALUES (1, 'sched'), (2, 'busy');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION d(w text) RETURNS bigint
								LANGUAGE sql
								AS '
								DELETE FROM test
								WHERE
									state = w
								RETURNING
									id;
							';"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("state", TEXT)],
                        rows: &[
                            &[T("1"), T("sched")],
                            &[T("2"), T("busy")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d('sched');",
                    expected: Expected::Rows {
                        columns: &[Column("d", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("state", TEXT)],
                        rows: &[
                            &[T("2"), T("busy")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple statements in function",
            set_up_script: &[
                "CREATE TABLE test (id int);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "CREATE VIEW test1 AS SELECT * FROM test WHERE id = 1;",
                "CREATE VIEW test2 AS SELECT * FROM test WHERE id = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION drop_views() RETURNS void
								LANGUAGE sql
								AS $$
							DROP VIEW test1;
							DROP VIEW test2;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT drop_views();",
                    expected: Expected::Rows {
                        columns: &[Column("drop_views", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "test1" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "test2" does not exist"#, position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function with default expression in parameter",
            set_up_script: &[
                "CREATE TABLE cp_test (a int, b text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION dfunc(e int, d text, f int default 100)
							 RETURNS int LANGUAGE SQL
							AS $$
								INSERT INTO cp_test VALUES(e+f, d);
								SELECT a FROM cp_test WHERE b = d;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION dfunc(e int, f int default 100)
							 RETURNS int LANGUAGE SQL
							AS $$
								INSERT INTO cp_test VALUES(e+f, 'seconddfunc');
								SELECT a FROM cp_test WHERE b = 'seconddfunc';
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dfunc(10, 'Hello', 20);",
                    expected: Expected::Rows {
                        columns: &[Column("dfunc", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cp_test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("30"), T("Hello")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dfunc(50, 'Bye');",
                    expected: Expected::Rows {
                        columns: &[Column("dfunc", INT4)],
                        rows: &[
                            &[T("150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cp_test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("30"), T("Hello")],
                            &[T("150"), T("Bye")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dfunc(2, 'After');",
                    expected: Expected::Rows {
                        columns: &[Column("dfunc", INT4)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cp_test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("30"), T("Hello")],
                            &[T("150"), T("Bye")],
                            &[T("102"), T("After")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION dfunc(e int, f text default '100')
							 RETURNS int LANGUAGE SQL
							AS $$
								INSERT INTO cp_test VALUES(e, f);
								SELECT a FROM cp_test WHERE b = f;
							$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dfunc(50);",
                    expected: Expected::Error(Diagnostic { code: "42725", message: "function dfunc(integer) is not unique", hint: "Could not choose a best candidate function. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "use sql statements in BEGIN ATOMIC ... END in sql_body",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION match_default() RETURNS jsonb
            LANGUAGE sql
            BEGIN ATOMIC 
				SELECT jsonb_build_object('k', 6, 'm', 2048, 'include_original', true, 'tokenizer', json_build_object('kind', 'ngram', 'token_length', 3), 'token_filters', json_build_array(json_build_object('kind', 'downcase'))) AS jsonb_build_object; 
			END;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.match_default();",
                    expected: Expected::Rows {
                        columns: &[Column("match_default", JSONB)],
                        rows: &[
                            &[T(r#"{"k": 6, "m": 2048, "tokenizer": {"kind": "ngram", "token_length": 3}, "token_filters": [{"kind": "downcase"}], "include_original": true}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION select1() RETURNS int
            LANGUAGE sql
            BEGIN ATOMIC 
				SELECT 1; 
			END;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT select1();",
                    expected: Expected::Rows {
                        columns: &[Column("select1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "use RETURN in BEGIN ATOMIC ... END in sql_body",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION return1() RETURNS text
            LANGUAGE sql
            BEGIN ATOMIC 
				RETURN 1::text || 'one'; 
			END;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT return1();",
                    expected: Expected::Rows {
                        columns: &[Column("return1", TEXT)],
                        rows: &[
                            &[T("1one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "user-defined composite array parameter and return type",
            set_up_script: &[
                "CREATE TYPE aggtype AS (a integer, b integer, c text)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION aggf_trans(aggtype[],integer,integer,text) RETURNS aggtype[] AS 'select array_append($1,ROW($2,$3,$4)::aggtype)' LANGUAGE sql STRICT IMMUTABLE",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aggf_trans(ARRAY[]::aggtype[], 1, 2, 'hello')",
                    expected: Expected::Rows {
                        columns: &[Column("aggf_trans", USER_DEFINED)],
                        rows: &[
                            &[T(r#"{"(1,2,hello)"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aggf_trans(ARRAY[ROW(1,2,'hello')::aggtype], 3, 4, 'world')",
                    expected: Expected::Rows {
                        columns: &[Column("aggf_trans", USER_DEFINED)],
                        rows: &[
                            &[T(r#"{"(1,2,hello)","(3,4,world)"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table function in FROM returning a table",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION figures() RETURNS TABLE(shape TEXT, sides INT) LANGUAGE SQL AS $$ SELECT 'triangle', 3 UNION ALL SELECT 'square', 4 $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM figures();",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT), Column("sides", INT4)],
                        rows: &[
                            &[T("triangle"), T("3")],
                            &[T("square"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shape, sides FROM figures();",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT), Column("sides", INT4)],
                        rows: &[
                            &[T("triangle"), T("3")],
                            &[T("square"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shape FROM figures() WHERE sides = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT)],
                        rows: &[
                            &[T("square")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM public.figures();",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT), Column("sides", INT4)],
                        rows: &[
                            &[T("triangle"), T("3")],
                            &[T("square"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM figures() AS f(name, edges);",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("edges", INT4)],
                        rows: &[
                            &[T("triangle"), T("3")],
                            &[T("square"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT figures();",
                    expected: Expected::Rows {
                        columns: &[Column("figures", RECORD)],
                        rows: &[
                            &[T("(triangle,3)")],
                            &[T("(square,4)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table function in FROM returning SETOF a table type",
            set_up_script: &[
                "CREATE TABLE shapes (shape TEXT, sides INT);",
                "INSERT INTO shapes VALUES ('triangle', 3), ('square', 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION with_sides(n INT) RETURNS SETOF shapes LANGUAGE SQL AS $$ SELECT * FROM shapes WHERE sides >= n $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM with_sides(4);",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT), Column("sides", INT4)],
                        rows: &[
                            &[T("square"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.shape FROM shapes s JOIN with_sides(3) w ON s.shape = w.shape ORDER BY s.shape;",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT)],
                        rows: &[
                            &[T("square")],
                            &[T("triangle")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT with_sides(4);",
                    expected: Expected::Rows {
                        columns: &[Column("with_sides", USER_DEFINED)],
                        rows: &[
                            &[T("(square,4)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table function in FROM returning a composite type",
            set_up_script: &[
                "CREATE TYPE figure AS (shape TEXT, sides INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION a_figure() RETURNS figure LANGUAGE SQL AS $$ SELECT ROW('triangle', 3)::figure $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM a_figure();",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT), Column("sides", INT4)],
                        rows: &[
                            &[T("triangle"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a_figure();",
                    expected: Expected::Rows {
                        columns: &[Column("a_figure", USER_DEFINED)],
                        rows: &[
                            &[T("(triangle,3)")],
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
fn test_sql_function_rules() {
    run_scripts(&[
        ScriptTest {
            name: "SQL function definitions and overloads",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';",
                    expected: Expected::Error(Diagnostic { code: "42723", message: r#"function "f" already exists with same argument types"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION f(int) RETURNS text LANGUAGE sql AS 'SELECT $1::text';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot change return type of existing function", hint: "Use DROP FUNCTION f(integer) first.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g(int = 1, int) RETURNS int LANGUAGE sql AS 'SELECT $1';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "input parameters after one with a default value must also have defaults", position: 28, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g(out a int, out b text) RETURNS int LANGUAGE sql AS 'SELECT 1, 2';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "function result type must be record because of OUT parameters", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS int LANGUAGE foo AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS int LANGUAGE sql AS 'SELECT ''a''::text';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return integer", detail: "Actual return type is text.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS int LANGUAGE sql AS 'SELECT 1, 2';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return integer", detail: "Final statement must return exactly one column.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS int LANGUAGE sql AS 'CREATE TABLE x (a int)';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return integer", detail: "Function's final statement must be SELECT or INSERT/UPDATE/DELETE/MERGE RETURNING.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS int LANGUAGE sql AS 'SELECT nope';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nope" does not exist"#, position: 57, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS void LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g() IS NULL;",
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
                    query: "CREATE FUNCTION h() RETURNS int LANGUAGE sql AS 'SELECT 1 WHERE false';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT h();",
                    expected: Expected::Rows {
                        columns: &[Column("h", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION h2() RETURNS SETOF int LANGUAGE sql AS 'SELECT 1 UNION SELECT 2 ORDER BY 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT h2();",
                    expected: Expected::Rows {
                        columns: &[Column("h2", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM h2();",
                    expected: Expected::Rows {
                        columns: &[Column("h2", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION k(a int, b int = 10) RETURNS int LANGUAGE sql AS 'SELECT a + b';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k(1), k(1, 2), k(b => 3, a => 1), k(1, b => 5);",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("k", INT4), Column("k", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("11"), T("3"), T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function k() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION k(a int) RETURNS int LANGUAGE sql AS 'SELECT a';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k(1);",
                    expected: Expected::Error(Diagnostic { code: "42725", message: "function k(integer) is not unique", hint: "Could not choose a best candidate function. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION m(a int, out b int, out c text) LANGUAGE sql AS 'SELECT a, ''x''';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT m(1);",
                    expected: Expected::Rows {
                        columns: &[Column("m", RECORD)],
                        rows: &[
                            &[T("(1,x)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM m(1);",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION n(a int) RETURNS TABLE(x int, y text) LANGUAGE sql AS 'SELECT a, ''q'' UNION ALL SELECT a + 1, ''r''';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n(1);",
                    expected: Expected::Rows {
                        columns: &[Column("n", RECORD)],
                        rows: &[
                            &[T("(1,q)")],
                            &[T("(2,r)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM n(1);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("1"), T("q")],
                            &[T("2"), T("r")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION one_column(a int) RETURNS TABLE(x int) LANGUAGE sql AS 'SELECT a UNION ALL SELECT a + 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT one_column(5);",
                    expected: Expected::Rows {
                        columns: &[Column("one_column", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_column(5);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s(x int) RETURNS int STRICT LANGUAGE sql AS 'SELECT 5';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s(NULL), s(1);",
                    expected: Expected::Rows {
                        columns: &[Column("s", INT4), Column("s", INT4)],
                        rows: &[
                            &[Null, T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r(x int) RETURNS int LANGUAGE sql RETURN x * 2;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r(4);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ba(x int) RETURNS int LANGUAGE sql BEGIN ATOMIC SELECT 1; SELECT x + 3; END;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ba(4);",
                    expected: Expected::Rows {
                        columns: &[Column("ba", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION concat_number(t text, n int) RETURNS text LANGUAGE sql AS 'SELECT t || n';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT concat_number('a', 1), 'b' || 2, 3 || 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("concat_number", TEXT), Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("a1"), T("b2"), T("3c")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(1), pg_typeof('a'::text), pg_typeof(ARRAY['x']), pg_typeof(now());",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE), Column("pg_typeof", REGTYPE), Column("pg_typeof", REGTYPE), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("integer"), T("text"), T("text[]"), T("timestamp with time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping functions",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f(text) RETURNS int LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION nope;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"could not find a function named "nope""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION nope(int);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nope(integer) does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION IF EXISTS nope(int);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    notices: &[Diagnostic { code: "00000", message: "function nope(pg_catalog.int4) does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION IF EXISTS nope;",
                    expected: Expected::Tag("DROP FUNCTION"),
                    notices: &[Diagnostic { code: "00000", message: "function nope() does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION f;",
                    expected: Expected::Error(Diagnostic { code: "42725", message: r#"function name "f" is not unique"#, hint: "Specify the argument list to select the function unambiguously.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION f(text), f(int);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f(1);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function f(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION p_like(int) RETURNS int LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE p_like(int);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "p_like(integer) is not a procedure", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROUTINE p_like(int);",
                    expected: Expected::Tag("DROP ROUTINE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function volatility",
            set_up_script: &[
                "CREATE FUNCTION vol_i() RETURNS int IMMUTABLE LANGUAGE sql AS 'SELECT 1';",
                "CREATE FUNCTION vol_s() RETURNS SETOF int STABLE LANGUAGE sql AS 'SELECT 2 UNION ALL SELECT 3';",
                "CREATE FUNCTION vol_v() RETURNS int LANGUAGE sql AS 'SELECT 4';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r"SELECT proname, provolatile FROM pg_proc WHERE proname LIKE 'vol\_%' ORDER BY proname;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("provolatile", CHAR)],
                        rows: &[
                            &[T("vol_i"), T("i")],
                            &[T("vol_s"), T("s")],
                            &[T("vol_v"), T("v")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vol_s() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("vol_s", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },    ]);
}

#[test]
fn test_polymorphic_functions() {
    run_scripts(&[
        ScriptTest {
            name: "Polymorphic SQL functions",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION range_add_bounds(anyrange) RETURNS anyelement AS 'SELECT lower($1) + upper($1)' LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_add_bounds(int4range(1, 17)), range_add_bounds(numrange(1.0001, 123.123));",
                    expected: Expected::Rows {
                        columns: &[Column("range_add_bounds", INT4), Column("range_add_bounds", NUMERIC)],
                        rows: &[
                            &[T("18"), T("124.1231")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rangetypes_sql(q anyrange, b anyarray, out c anyelement) AS $$ SELECT upper($1) + $2[1] $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rangetypes_sql(int4range(1,10), ARRAY[2,20]);",
                    expected: Expected::Rows {
                        columns: &[Column("rangetypes_sql", INT4)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rangetypes_sql(numrange(1,10), ARRAY[2,20]);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function rangetypes_sql(numrange, integer[]) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bogus_func(anyelement) RETURNS anyrange AS 'SELECT int4range(1,10)' LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot determine result data type", detail: "A result of type anyrange requires at least one input of type anyrange or anymultirange.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bogus_func(int) RETURNS anyrange AS 'SELECT int4range(1,10)' LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot determine result data type", detail: "A result of type anyrange requires at least one input of type anyrange or anymultirange.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION polyf(x anyelement) RETURNS anyelement AS $$ SELECT x + 1 $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polyf(5), polyf(5.5);",
                    expected: Expected::Rows {
                        columns: &[Column("polyf", INT4), Column("polyf", NUMERIC)],
                        rows: &[
                            &[T("6"), T("6.5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT polyf('a');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "could not determine polymorphic type because input has type unknown", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_running_sql_functions() {
    run_scripts(&[
        ScriptTest {
            name: "SQL functions run their queries with their arguments inside the calling statement",
            set_up_script: &[
                r#"CREATE TABLE sf (a INT PRIMARY KEY, b TEXT);"#,
                r#"INSERT INTO sf VALUES (1, 'one'), (2, 'two'), (3, 'three');"#,
                r#"CREATE FUNCTION sf_name(x INT) RETURNS TEXT LANGUAGE sql STABLE AS 'SELECT b FROM sf WHERE a = x';"#,
                r#"CREATE FUNCTION sf_first() RETURNS INT LANGUAGE sql AS 'SELECT a FROM sf ORDER BY a DESC';"#,
                r#"CREATE FUNCTION sf_add(x INT, y TEXT) RETURNS BIGINT LANGUAGE sql AS 'INSERT INTO sf VALUES (x, y); SELECT count(*) FROM sf';"#,
                r#"CREATE FUNCTION sf_ins_stable(x INT) RETURNS INT LANGUAGE sql STABLE AS 'INSERT INTO sf VALUES (x, ''s'') RETURNING a';"#,
                r#"CREATE FUNCTION sf_div(x INT) RETURNS INT LANGUAGE sql AS 'SELECT 1; SELECT 10 / x';"#,
                r#"CREATE FUNCTION sf_void(x INT) RETURNS VOID LANGUAGE sql AS 'UPDATE sf SET b = upper(b) WHERE a = x';"#,
                r#"CREATE FUNCTION sf_strict(x INT) RETURNS INT LANGUAGE sql STRICT AS 'SELECT count(*)::int FROM sf WHERE a > x';"#,
                r#"CREATE FUNCTION sf_poly(x anyelement) RETURNS anyelement LANGUAGE sql AS 'SELECT x FROM sf LIMIT 1';"#,
                r#"CREATE FUNCTION sf_atomic(x INT) RETURNS INT LANGUAGE sql BEGIN ATOMIC INSERT INTO sf VALUES (x, 'atomic'); SELECT max(a) FROM sf; END;"#,
                r#"CREATE FUNCTION sf_fact(n INT) RETURNS NUMERIC LANGUAGE sql AS 'SELECT CASE WHEN n <= 1 THEN 1::numeric ELSE n * sf_fact(n - 1) END FROM sf LIMIT 1';"#,
                r#"CREATE FUNCTION sf_imm(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT x + v FROM (VALUES (1)) t(v)';"#,
                r#"SET check_function_bodies = off;"#,
                r#"CREATE FUNCTION sf_commit() RETURNS INT LANGUAGE sql AS 'COMMIT; SELECT 1';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT sf_name(2), sf_name(4) IS NULL AS missing;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_name", TEXT), Column("missing", BOOL)],
                        rows: &[
                            &[T("two"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a, sf_name(a) FROM sf ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("sf_name", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_first();"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_first", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_add(4, 'four');"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_add", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM sf;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_ins_stable(9);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"INSERT is not allowed in a non-volatile function"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_div(5);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_div", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_div(0);"#,
                    expected: Expected::Error(Diagnostic { code: "22012", message: r#"division by zero"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_void(1);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_void", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT b FROM sf WHERE a = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("ONE")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_strict(NULL) IS NULL AS isnull, sf_strict(1);"#,
                    expected: Expected::Rows {
                        columns: &[Column("isnull", BOOL), Column("sf_strict", INT4)],
                        rows: &[
                            &[T("t"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_poly(5), sf_poly('t'::text);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_poly", INT4), Column("sf_poly", TEXT)],
                        rows: &[
                            &[T("5"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_atomic(10);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_atomic", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_fact(10);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_fact", NUMERIC)],
                        rows: &[
                            &[T("3628800")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_commit();"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"COMMIT is not allowed in an SQL function"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sf_imm(2);"#,
                    expected: Expected::Rows {
                        columns: &[Column("sf_imm", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM sf WHERE a = sf_imm(1);"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
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
fn test_function_set_clauses() {
    run_scripts(&[
        ScriptTest {
            name: "a routine's SET clauses apply while it runs, through CREATE and ALTER FUNCTION, pg_get_functiondef, triggers, set-returning functions, and CALL",
            set_up_script: &[
                "CREATE SCHEMA sa;",
                "CREATE SCHEMA sb;",
                "CREATE TABLE sa.t (v TEXT);",
                "CREATE TABLE sb.t (v TEXT);",
                "INSERT INTO sa.t VALUES ('in sa');",
                "INSERT INTO sb.t VALUES ('in sb');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION wm() RETURNS TEXT LANGUAGE sql SET work_mem = '128kB' AS $$ SELECT current_setting('work_mem') $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT wm(), current_setting('work_mem');",
                    expected: Expected::Rows {
                        columns: &[Column("wm", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("128kB"), T("4MB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'wm';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{work_mem=128kB}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp() RETURNS TEXT LANGUAGE sql SET search_path = sb, public AS $$ SELECT v FROM t $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp();",
                    expected: Expected::Rows {
                        columns: &[Column("sp", TEXT)],
                        rows: &[
                            &[T("in sb")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = sa, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp(), (SELECT v FROM t);",
                    expected: Expected::Rows {
                        columns: &[Column("sp", TEXT), Column("v", TEXT)],
                        rows: &[
                            &[T("in sb"), T("in sa")],
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
                    query: "SELECT pg_get_functiondef('sp'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION public.sp()\n RETURNS text\n LANGUAGE sql\n SET search_path TO 'sb', 'public'\nAS $function$ SELECT v FROM t $function$\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '2MB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fc() RETURNS TEXT LANGUAGE sql SET work_mem FROM CURRENT AS $$ SELECT current_setting('work_mem') $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fc(), proconfig FROM pg_proc WHERE proname = 'fc';",
                    expected: Expected::Rows {
                        columns: &[Column("fc", TEXT), Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("2MB"), T("{work_mem=2MB}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION wm() SET enable_seqscan = off;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION wm() SET work_mem = '256kB';",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'wm';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{work_mem=256kB,enable_seqscan=off}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION wm() RESET work_mem;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'wm';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{enable_seqscan=off}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION wm() RESET ALL;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'wm';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION plw() RETURNS TEXT LANGUAGE plpgsql SET work_mem = '100kB' AS $$ BEGIN RETURN current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT plw(), current_setting('work_mem');",
                    expected: Expected::Rows {
                        columns: &[Column("plw", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("100kB"), T("4MB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION plerr() RETURNS TEXT LANGUAGE plpgsql SET work_mem = '100kB' AS $$ BEGIN RAISE EXCEPTION 'boom %', current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT plerr();",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "boom 100kB", context: Some("PL/pgSQL function plerr() line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("4MB")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION plset() RETURNS TEXT LANGUAGE plpgsql SET work_mem = '100kB' AS $$ BEGIN SET work_mem = '300kB'; SET enable_hashjoin = off; RETURN current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT plset();",
                    expected: Expected::Rows {
                        columns: &[Column("plset", TEXT)],
                        rows: &[
                            &[T("300kB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("300kB")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW enable_hashjoin;",
                    expected: Expected::Rows {
                        columns: &[Column("enable_hashjoin", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION srf() RETURNS SETOF TEXT LANGUAGE sql SET work_mem = '111kB' AS $$ SELECT current_setting('work_mem') UNION ALL SELECT current_setting('work_mem') $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM srf();",
                    expected: Expected::Rows {
                        columns: &[Column("srf", TEXT)],
                        rows: &[
                            &[T("111kB")],
                            &[T("111kB")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT srf();",
                    expected: Expected::Rows {
                        columns: &[Column("srf", TEXT)],
                        rows: &[
                            &[T("111kB")],
                            &[T("111kB")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE trg (v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trgf() RETURNS TRIGGER LANGUAGE plpgsql SET work_mem = '222kB' AS $$ BEGIN NEW.v := current_setting('work_mem'); RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trg_b BEFORE INSERT ON trg FOR EACH ROW EXECUTE FUNCTION trgf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO trg VALUES ('x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, current_setting('work_mem') FROM trg;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("222kB"), T("300kB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE pc() LANGUAGE plpgsql SET work_mem = '333kB' AS $$ BEGIN RAISE NOTICE '%', current_setting('work_mem'); COMMIT; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL pc();",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", context: Some("PL/pgSQL function pc() line 1 at COMMIT"), ..E }),
                    notices: &[Diagnostic { code: "00000", message: "333kB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE pd() LANGUAGE plpgsql SET work_mem = '333kB' AS $$ BEGIN RAISE NOTICE '%', current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CALL pd();"#,
                    expected: Expected::Tag("CALL"),
                    notices: &[Diagnostic { code: "00000", message: "333kB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("300kB")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad1() RETURNS INT LANGUAGE sql SET no_such_setting = 1 AS $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "unrecognized configuration parameter \"no_such_setting\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad2() RETURNS INT LANGUAGE sql SET work_mem = 'lots' AS $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for parameter \"work_mem\": \"lots\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad3() RETURNS INT LANGUAGE sql SET shared_buffers = '1MB' AS $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "55P02", message: "parameter \"shared_buffers\" cannot be changed without restarting the server", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION cust() RETURNS TEXT LANGUAGE sql SET my.var = 'hello' AS $$ SELECT current_setting('my.var') $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cust();",
                    expected: Expected::Rows {
                        columns: &[Column("cust", TEXT)],
                        rows: &[
                            &[T("hello")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION res() RETURNS INT LANGUAGE sql SET work_mem = '1MB' RESET work_mem AS $$ SELECT 1 $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'res';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION inl(x INT) RETURNS INT LANGUAGE sql SET work_mem = '1MB' AS $$ SELECT x + 1 $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT inl(a) FROM (VALUES (1)) v(a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                            &[T("  Output: inl(1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION secd() RETURNS TEXT LANGUAGE sql SECURITY DEFINER SET search_path = sb AS $$ SELECT v FROM t $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT secd();",
                    expected: Expected::Rows {
                        columns: &[Column("secd", TEXT)],
                        rows: &[
                            &[T("in sb")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_functiondef('secd'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION public.secd()\n RETURNS text\n LANGUAGE sql\n SECURITY DEFINER\n SET search_path TO 'sb'\nAS $function$ SELECT v FROM t $function$\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp2() RETURNS TEXT LANGUAGE sql SET search_path TO \"$user\", public, sa SET DateStyle = 'ISO, DMY' AS $$ SELECT 1::text $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE proname = 'sp2';",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{\"search_path=\\\"$user\\\", public, sa\",\"DateStyle=ISO, DMY\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_functiondef('sp2'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION public.sp2()\n RETURNS text\n LANGUAGE sql\n SET search_path TO '$user', 'public', 'sa'\n SET \"DateStyle\" TO 'ISO, DMY'\nAS $function$ SELECT 1::text $function$\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_sql_functions_returning_row_types_with_dropped_columns() {
    run_scripts(&[
        ScriptTest {
            name: "a SQL function returning a table's row type that has a dropped column",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT, c INT, d INT);",
                "ALTER TABLE t DROP COLUMN b;",
                "CREATE FUNCTION ft() RETURNS SETOF t LANGUAGE sql AS 'SELECT 1, 2, 3';",
                "CREATE FUNCTION f1() RETURNS t LANGUAGE sql AS 'SELECT 4, 5, 6';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT c, d FROM ft();",
                    expected: Expected::Rows {
                        columns: &[Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ft();",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ft()).d, (f1()).c;",
                    expected: Expected::Rows {
                        columns: &[Column("d", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("5")],
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
fn test_sql_function_utility_statements() {
    run_scripts(&[
        ScriptTest {
            name: "SQL function bodies refuse utility statements in BEGIN ATOMIC and CALLs with output arguments",
            set_up_script: &[
                "CREATE PROCEDURE out_proc(OUT a INTEGER) LANGUAGE sql AS $$ SELECT 1 $$;",
                "CREATE PROCEDURE in_proc(a INTEGER) LANGUAGE sql AS $$ SELECT a $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION atomic_vacuum() RETURNS void LANGUAGE sql BEGIN ATOMIC VACUUM; END;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "VACUUM is not yet supported in unquoted SQL function body", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION atomic_notify() RETURNS void LANGUAGE sql BEGIN ATOMIC SELECT 1; NOTIFY x; END;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "NOTIFY is not yet supported in unquoted SQL function body", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION calls_out() RETURNS void LANGUAGE sql AS $$ CALL out_proc(NULL) $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "calling procedures with output arguments is not supported in SQL functions", context: Some(r#"SQL function "calls_out""#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION calls_out2() RETURNS INTEGER LANGUAGE sql AS $$ SELECT 1; CALL out_proc(NULL); SELECT 2 $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "calling procedures with output arguments is not supported in SQL functions", context: Some(r#"SQL function "calls_out2""#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION calls_in() RETURNS void LANGUAGE sql AS $$ CALL in_proc(1) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION calls_out3() RETURNS void LANGUAGE sql AS $$ CALL out_proc(NULL) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_utility_statements_in_sql_functions() {
    run_scripts(&[
        ScriptTest {
            name: "a SQL function runs its utility statements, which a later statement sees, and refuses them when not volatile or when they end a function that returns a value",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION make_and_fill() RETURNS BIGINT LANGUAGE sql AS $$ CREATE TABLE made (a INT); INSERT INTO made VALUES (1), (2); SELECT count(*) FROM made; $$;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "made" does not exist"#, position: 106, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION make_and_fill() RETURNS BIGINT LANGUAGE sql AS $$ CREATE TABLE made (a INT); INSERT INTO made VALUES (1), (2); SELECT count(*) FROM made; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ends_in_utility() RETURNS INT LANGUAGE sql AS $$ CREATE TABLE e (a INT) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT make_and_fill();",
                    expected: Expected::Rows {
                        columns: &[Column("make_and_fill", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM made;",
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
                    query: "CREATE FUNCTION stable_utility() RETURNS VOID LANGUAGE sql STABLE AS $$ CREATE TABLE s (a INT) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stable_utility();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CREATE TABLE is not allowed in a non-volatile function", context: Some(r#"SQL function "stable_utility" statement 1"#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ends_in_utility2() RETURNS INT LANGUAGE sql AS $$ CREATE TABLE e (a INT) $$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return integer", detail: "Function's final statement must be SELECT or INSERT/UPDATE/DELETE/MERGE RETURNING.", context: Some(r#"SQL function "ends_in_utility2""#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ends_in_utility();",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return integer", detail: "Function's final statement must be SELECT or INSERT/UPDATE/DELETE/MERGE RETURNING.", context: Some(r#"SQL function "ends_in_utility" statement 1"#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION void_utility() RETURNS VOID LANGUAGE sql AS $$ CREATE TABLE v (a INT) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT void_utility();",
                    expected: Expected::Rows {
                        columns: &[Column("void_utility", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM v;",
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

#[test]
fn test_sql_function_composite_parameter_star() {
    run_scripts(&[
        ScriptTest {
            name: "a SQL function's composite parameter written v.* expands to its fields",
            set_up_script: &[
                "CREATE TYPE fstar_compos AS (f1 int, f2 text)",
                "CREATE TABLE fstar_t OF fstar_compos",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fstar1(v fstar_compos) RETURNS void AS $$ INSERT INTO fstar_t VALUES (v.*); $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fstar2(v fstar_compos) RETURNS void AS $$ INSERT INTO fstar_t SELECT fstar2.v.*; $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fstar3(v fstar_compos) RETURNS fstar_compos AS $$ SELECT v.*; $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fstar1(ROW(1, 'one')), fstar2(ROW(2, 'two'));",
                    expected: Expected::Rows {
                        columns: &[Column("fstar1", VOID), Column("fstar2", VOID)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fstar_t ORDER BY f1;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (fstar3(ROW(3, 'three'))).*;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("3"), T("three")],
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
fn test_sql_function_call_with_output_arguments_at_execution() {
    run_scripts(&[
        ScriptTest {
            name: "a SQL function created without checking its body fails when it reaches a CALL with output arguments",
            set_up_script: &[
                "CREATE PROCEDURE xout_proc(INOUT x int) LANGUAGE sql AS $$ SELECT 1 $$",
                "SET check_function_bodies = off",
                "CREATE FUNCTION xcalls_out() RETURNS int LANGUAGE sql AS $$ CALL xout_proc(NULL); SELECT 1 $$",
                "CREATE FUNCTION xcalls_out2() RETURNS int LANGUAGE sql AS $$ SELECT 1; CALL xout_proc(NULL); SELECT 2 $$",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT xcalls_out();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "calling procedures with output arguments is not supported in SQL functions", context: Some(r#"SQL function "xcalls_out" statement 1"#), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT xcalls_out2();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "calling procedures with output arguments is not supported in SQL functions", context: Some(r#"SQL function "xcalls_out2" statement 2"#), ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_function_after_waiting_for_a_concurrent_drop() {
    run_scripts(&[
        ScriptTest {
            name: "a function made in a schema another transaction dropped fails on its dependency",
            set_up_script: &[
                "CREATE SCHEMA testschema;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA testschema;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION testschema.foo() RETURNS INT AS 'select 1' LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "referenced schema was concurrently dropped", ..E }),
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
            ],
            ..S
        },
        ScriptTest {
            name: "a function taking a type another transaction dropped fails on its dependency",
            set_up_script: &[
                "CREATE TYPE foo AS ENUM ('one', 'two');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE foo;",
                    expected: Expected::Tag("DROP TYPE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fooargtype(num foo) RETURNS INT AS 'select 1' LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "referenced type was concurrently dropped", ..E }),
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
            ],
            ..S
        },
        ScriptTest {
            name: "a function calling a function another transaction dropped fails on its dependency",
            set_up_script: &[
                "CREATE FUNCTION f() RETURNS INT LANGUAGE sql RETURN 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION f();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION foofunc() RETURNS INT LANGUAGE sql RETURN f() + 1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "referenced function was concurrently dropped", ..E }),
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
            ],
            ..S
        },
        ScriptTest {
            name: "a function made in a schema another transaction is dropping times out on its dependency's lock",
            set_up_script: &["CREATE SCHEMA testschema;"],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA testschema;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION testschema.foo() RETURNS INT AS 'select 1' LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OR REPLACE FUNCTION and ALTER FUNCTION change a built-in function's row",
            set_up_script: &["CREATE TABLE t (a TEXT);", "INSERT INTO t VALUES ('ABC');"],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.lower(text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT 'x' $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower('ABC'), lower(a) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("lower", TEXT), Column("lower", TEXT)],
                        rows: &[
                            &[T("abc"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proisstrict, prosrc FROM pg_proc WHERE oid = 'lower(text)'::regprocedure;",
                    expected: Expected::Rows {
                        columns: &[Column("proisstrict", BOOL), Column("prosrc", TEXT)],
                        rows: &[
                            &[T("f"), T(" SELECT 'x' ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_functiondef('lower(text)'::regprocedure);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION pg_catalog.lower(text)\n RETURNS text\n LANGUAGE sql\n IMMUTABLE\nAS $function$ SELECT 'x' $function$\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.obj_description(oid, name) RETURNS text LANGUAGE sql STABLE STRICT AS $$ SELECT 'replaced' $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(1, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("replaced")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION pg_catalog.col_description(oid, integer) SET search_path = public;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proconfig FROM pg_proc WHERE oid = 'col_description(oid,integer)'::regprocedure;",
                    expected: Expected::Rows {
                        columns: &[Column("proconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{search_path=public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION pg_catalog.length(text) STABLE COST 7;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT provolatile, procost FROM pg_proc WHERE oid = 'length(text)'::regprocedure;",
                    expected: Expected::Rows {
                        columns: &[Column("provolatile", CHAR), Column("procost", FLOAT4)],
                        rows: &[
                            &[T("s"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.lower(text) RETURNS integer LANGUAGE sql AS $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot change return type of existing function", hint: "Use DROP FUNCTION lower(text) first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.lower(x text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT 'named ' || x $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(x => 'Q');",
                    expected: Expected::Rows {
                        columns: &[Column("lower", TEXT)],
                        rows: &[
                            &[T("q")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.upper(text) RETURNS text LANGUAGE sql IMMUTABLE BEGIN ATOMIC SELECT a FROM t; END;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE objid = 'upper(text)'::regprocedure;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("function upper(text)"), T("column a of table t"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop function upper(text) because it is required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE r1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION pg_catalog.lower(text) RETURNS text LANGUAGE sql AS $$ SELECT 'y' $$;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema pg_catalog", ..E }),
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
fn test_set_clause_of_a_setting_that_cannot_be_saved() {
    run_scripts(&[
        ScriptTest {
            name: "a function's SET clause refuses transaction_read_only, which cannot be set locally in functions",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION errfunc() RETURNS int LANGUAGE SQL AS 'SELECT 1' SET transaction_read_only = on;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"parameter "transaction_read_only" cannot be set locally in functions"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION okfunc() RETURNS int LANGUAGE SQL AS 'SELECT 1' SET transaction_read_only = on;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT okfunc();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"parameter "transaction_read_only" cannot be set locally in functions"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_read_only;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_read_only", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
