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
fn test_create_procedure_language_plpgsql() {
    run_scripts(&[
        ScriptTest {
            name: "Simple example",
            set_up_script: &[
                "CREATE TABLE test (v1 INT8);",
                r#"CREATE PROCEDURE example(input INT8) AS $$
				BEGIN
					INSERT INTO test VALUES (input);
				END;
				$$ LANGUAGE 'plpgsql';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL example(1);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL example('2');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHILE Label",
            set_up_script: &[
                "CREATE TABLE test (v1 INT8);",
                r#"CREATE PROCEDURE interpreted_while_label(input INT4) AS $$
DECLARE
	counter INT4;
BEGIN
	<<while_label>>
	WHILE input < 1000 LOOP
		input := input + 1;
		counter := counter + 1;
		IF counter >= 10 THEN
			EXIT while_label;
		END IF;
	END LOOP;
	INSERT INTO test VALUES (input);
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL interpreted_while_label(42);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Overloading",
            set_up_script: &[
                "CREATE TABLE test (v1 TEXT);",
                r#"CREATE PROCEDURE interpreted_overload(input TEXT) AS $$
DECLARE
	var1 TEXT;
BEGIN
	IF length(input) > 3 THEN
		var1 := input || '_long';
	ELSE
		var1 := input;
	END IF;
	INSERT INTO test VALUES (var1);
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE PROCEDURE interpreted_overload(input INT4) AS $$
DECLARE
	var1 INT4;
BEGIN
	IF input > 3 THEN
		var1 := -input;
	ELSE
		var1 := input;
	END IF;
	INSERT INTO test VALUES (var1::text);
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL interpreted_overload('abc');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_overload('abcd');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_overload(3);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_overload(4);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", TEXT)],
                        rows: &[
                            &[T("abc")],
                            &[T("abcd_long")],
                            &[T("3")],
                            &[T("-4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Branching",
            set_up_script: &[
                "CREATE TABLE test(v1 INT4, v2 INT4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE PROCEDURE interpreted_branch(input INT4) AS $$
BEGIN
	DELETE FROM test WHERE v1 = 1;
	INSERT INTO test VALUES (1, input + 100);
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_branch(4);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("104")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE v1 = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE PROCEDURE interpreted_branch(input INT4) AS $$
BEGIN
	DELETE FROM test WHERE v1 = 2;
	INSERT INTO test VALUES (2, input + 1000);
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'updated func')::text) = 32;",
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
                    query: "CALL interpreted_branch(56);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("2"), T("1056")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE v1 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_branch(57);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("157")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merging No Conflict",
            set_up_script: &[
                "CREATE TABLE test(v1 INT4, v2 INT4);",
                "INSERT INTO test VALUES (1, 77);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE PROCEDURE interpreted_merging(input TEXT) AS $$
BEGIN
	DELETE FROM test WHERE v1 = 2;
	INSERT INTO test VALUES (2, input::int4 + 100);
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_merging('12');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("77")],
                            &[T("2"), T("112")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_merging(55);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure interpreted_merging(integer) does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE PROCEDURE interpreted_merging(input INT4) AS $$
BEGIN
	DELETE FROM test WHERE v1 = 3;
	INSERT INTO test VALUES (3, input::int4 + 1000);
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'another func')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "CALL interpreted_merging(55);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure interpreted_merging(integer) does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE PROCEDURE interpreted_merging(input TEXT) AS $$
BEGIN
	DELETE FROM test WHERE v1 = 2;
	INSERT INTO test VALUES (2, input::int4 + 10000);
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'updated table')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_merge('other')::text) = 57;",
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
                    query: "CALL interpreted_merging('33');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_merging(77);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("77")],
                            &[T("3"), T("1077")],
                            &[T("2"), T("10033")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"Procedure updates "definition" with custom body"#,
            set_up_script: &[
                "CREATE TABLE test (v1 TEXT);",
                "CREATE PROCEDURE interpreted_example(input TEXT) AS $$ BEGIN INSERT INTO test VALUES ('1' || input); END; $$ LANGUAGE plpgsql;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE interpreted_example(input TEXT) AS $$ BEGIN INSERT INTO test VALUES ('3' || input); END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'other')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE interpreted_example(input TEXT) AS $$ BEGIN INSERT INTO test VALUES ('2' || input); END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.interpreted_example(text)"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'next')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.interpreted_example(text)"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: r#"SELECT base_value, our_value, dolt_our_diff_type, their_value, dolt_their_diff_type, dolt_conflict_id FROM "dolt_conflicts_interpreted_example(text)";"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_value", TEXT), Column("our_value", TEXT), Column("dolt_our_diff_type", TEXT), Column("their_value", TEXT), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                            &[T("BEGIN INSERT INTO test VALUES ('1' || input); END;"), T("BEGIN INSERT INTO test VALUES ('2' || input); END;"), T("modified"), T("BEGIN INSERT INTO test VALUES ('3' || input); END;"), T("modified"), T("definition")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: r#"UPDATE "dolt_conflicts_interpreted_example(text)" SET our_value = 'BEGIN INSERT INTO test VALUES (''7'' || input); END;' WHERE dolt_conflict_id = 'definition';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: r#"DELETE FROM "dolt_conflicts_interpreted_example(text)" WHERE dolt_conflict_id = 'definition';"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: r#"SELECT * FROM "dolt_conflicts_interpreted_example(text)";"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "dolt_conflicts_interpreted_example(text)" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL interpreted_example('12');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", TEXT)],
                        rows: &[
                            &[T("712")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DECLARE variable with default value of literal value or parameter reference",
            set_up_script: &[
                "CREATE TABLE t (a int, b text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE PROCEDURE m (x text, y text, e int) LANGUAGE plpgsql AS $$
declare
  xx text := x;
  yy text := y;
  zz int := e;
begin
  insert into t values (zz, xx || yy);
end
$$;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL m('a', 'B', 1);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("aB")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "use nested block statements and call statement in procedure body",
            set_up_script: &[
                "CREATE TABLE tbl (a int, b text);",
                r#"CREATE PROCEDURE add_value(IN a int, IN b text)
            LANGUAGE plpgsql
            AS $$
        BEGIN
            INSERT INTO tbl VALUES (a, b);
        END;
		$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE PROCEDURE check_and_add(IN i int, IN t text)
            LANGUAGE plpgsql
            AS $$
		DECLARE d text := t;
        BEGIN
            IF LENGTH(t) < 6 THEN
                d = t || ' is too short';
            END IF;

            BEGIN
                CALL add_value(i, d);
            END;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL check_and_add(1, 'hi');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL check_and_add(3, 'hellooo');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("hi is too short")],
                            &[T("3"), T("hellooo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_procedure_and_do_rules() {
    run_scripts(&[
        ScriptTest {
            name: "procedures and DO",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE p(a int, OUT b int) LANGUAGE plpgsql AS $$ BEGIN b := a + 1; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE p(a int, OUT b int) LANGUAGE plpgsql AS $$ BEGIN b := a + 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42723", message: r#"function "p" already exists with same argument types"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(1, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL p(1);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure p(integer) does not exist", hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.", position: 6, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p(1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "p(integer) is a procedure", hint: "To call a procedure, use CALL.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE q(INOUT x int, y text = 'd') LANGUAGE sql AS $$ SELECT x * 2 $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL q(4);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL q(4, 'z');",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE r() LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'hi'; RETURN; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL r();",
                    expected: Expected::Tag("CALL"),
                    notices: &[Diagnostic { code: "00000", message: "hi", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE r;",
                    expected: Expected::Tag("DROP PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE nope();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "procedure nope() does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION p(int, int);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function p(integer, integer) does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE p(int);",
                    expected: Expected::Tag("DROP PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fx() RETURNS int LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL fx();",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "fx() is not a procedure", hint: "To call a function, use SELECT.", position: 6, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE fx();",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "fx() is not a procedure", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL abs(1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "abs(integer) is not a procedure", hint: "To call a function, use SELECT.", position: 6, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE s(a int) LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "RETURN cannot have a parameter in a procedure", position: 63, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE logged (v text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE log_it(v text) LANGUAGE plpgsql AS $$ BEGIN INSERT INTO logged VALUES (v); END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL log_it('first');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN RAISE NOTICE 'x %', 1; INSERT INTO logged VALUES ('from do'); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "x 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE plpgsql $$ DECLARE n int; BEGIN SELECT count(*) INTO n FROM logged; RAISE NOTICE 'rows %', n; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "rows 2", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE sql $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"language "sql" does not support inline code execution"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO LANGUAGE missing_language $$ SELECT 1 $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "missing_language" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM logged ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("first")],
                            &[T("from do")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_call_procedures() {
    run_scripts(&[
        ScriptTest {
            name: "CALL runs procedures with defaults, named and variadic arguments, and output arguments",
            set_up_script: &[
                "CREATE TABLE ct (a INT, b TEXT);",
                "CREATE PROCEDURE cp_ins(x INT, y TEXT DEFAULT 'd') LANGUAGE plpgsql AS $$ BEGIN INSERT INTO ct VALUES (x, y); END $$;",
                "CREATE PROCEDURE cp_sql(x INT) LANGUAGE sql AS $$ INSERT INTO ct VALUES (x, 'sql') $$;",
                "CREATE PROCEDURE cp_out(IN x INT, OUT doubled INT, INOUT tag TEXT) LANGUAGE plpgsql AS $$ BEGIN doubled := x * 2; tag := tag || '!'; END $$;",
                "CREATE PROCEDURE cp_poly(INOUT v anyelement) LANGUAGE plpgsql AS $$ BEGIN v := v; END $$;",
                "CREATE PROCEDURE cp_var(VARIADIC xs INT[]) LANGUAGE plpgsql AS $$ BEGIN INSERT INTO ct VALUES (array_length(xs, 1), 'var'); END $$;",
                "CREATE FUNCTION cf(x INT) RETURNS INT LANGUAGE sql AS 'SELECT x';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL cp_ins(1);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_ins(2, 'two');",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_ins(y => 'named', x => 3);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_sql(4);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_var(5, 6, 7);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ct ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("d")],
                            &[T("2"), T("two")],
                            &[T("3"), T("named")],
                            &[T("3"), T("var")],
                            &[T("4"), T("sql")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_out(21, NULL, 'hi');",
                    expected: Expected::Rows {
                        columns: &[Column("doubled", INT4), Column("tag", TEXT)],
                        rows: &[
                            &[T("42"), T("hi!")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_out(x => 4, doubled => NULL, tag => 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("doubled", INT4), Column("tag", TEXT)],
                        rows: &[
                            &[T("8"), T("a!")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_poly(1.5);",
                    expected: Expected::Rows {
                        columns: &[Column("v", NUMERIC)],
                        rows: &[
                            &[T("1.5")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_poly('x'::TEXT);",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("x")],
                        ],
                        tag: "CALL",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cf(1);",
                    expected: Expected::Error(Diagnostic {
                        code: "42809",
                        message: "cf(integer) is not a procedure",
                        hint: "To call a function, use SELECT.",
                        position: 6,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL nope(1);",
                    expected: Expected::Error(Diagnostic {
                        code: "42883",
                        message: "procedure nope(integer) does not exist",
                        hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.",
                        position: 6,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_ins();",
                    expected: Expected::Error(Diagnostic {
                        code: "42883",
                        message: "procedure cp_ins() does not exist",
                        hint: "No procedure matches the given name and argument types. You might need to add explicit type casts.",
                        position: 6,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_ins(1 + (SELECT 1));",
                    expected: Expected::Error(Diagnostic {
                        code: "0A000",
                        message: "cannot use subquery in CALL argument",
                        position: 17,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL cp_ins(8);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ct;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL's CALL assigns a procedure's output arguments to the variables passed for them",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT);",
                "CREATE PROCEDURE inner_p(IN x INT, OUT doubled INT, INOUT tag TEXT) LANGUAGE plpgsql AS $$ BEGIN doubled := x * 2; tag := tag || '!'; END $$;",
                "CREATE PROCEDURE outer_p(n INT) LANGUAGE plpgsql AS $$ DECLARE d INT; t TEXT := 'a'; BEGIN FOR i IN 1..n LOOP CALL inner_p(i, d, t); INSERT INTO pt VALUES (d, t); END LOOP; END $$;",
                "CREATE FUNCTION f_calls() RETURNS INT LANGUAGE plpgsql AS $$ DECLARE d INT; t TEXT := 'f'; BEGIN CALL inner_p(7, d, t); RETURN d; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL outer_p(3);",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("a!")],
                            &[T("4"), T("a!!")],
                            &[T("6"), T("a!!!")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE d INT; t TEXT := 'x'; BEGIN CALL inner_p(5, d, t); RAISE NOTICE 'd=% t=%', d, t; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "d=10 t=x!", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE d INT; BEGIN CALL inner_p(5, d, 'lit'); END $$;",
                    expected: Expected::Error(Diagnostic {
                        code: "42601",
                        message: r#"procedure parameter "tag" is an output parameter but corresponding argument is not writable"#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE d INT; t CONSTANT TEXT := 'c'; BEGIN CALL inner_p(5, d, t); END $$;",
                    expected: Expected::Error(Diagnostic { code: "22005", message: r#"variable "t" is declared CONSTANT"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DO 'BEGIN RAISE NOTICE ''nested''; END'; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "nested", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_calls();",
                    expected: Expected::Rows {
                        columns: &[Column("f_calls", INT4)],
                        rows: &[
                            &[T("14")],
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
fn test_procedure_transaction_control() {
    run_scripts(&[
        ScriptTest {
            name: "COMMIT and ROLLBACK in DO blocks and procedures end the transaction and start the next",
            set_up_script: &[
                "CREATE TABLE tt (a INT);",
                "CREATE PROCEDURE tp_commit() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO tt VALUES (1); COMMIT; END $$;",
                "CREATE FUNCTION tf_calls() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN CALL tp_commit(); RETURN 1; END $$;",
                "CREATE PROCEDURE tp_only() LANGUAGE plpgsql AS $$ BEGIN COMMIT; END $$;",
                "CREATE FUNCTION tf_par() RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN CALL tp_only(); RETURN 1; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO tt VALUES (1); COMMIT; INSERT INTO tt VALUES (2); ROLLBACK; INSERT INTO tt VALUES (3); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET LOCAL work_mem = '77kB'; RAISE NOTICE 'before %', current_setting('work_mem'); COMMIT; RAISE NOTICE 'after %', current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "before 77kB", ..N }, Diagnostic { code: "00000", message: "after 4MB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET LOCAL work_mem = '77kB'; ROLLBACK; RAISE NOTICE 'after rollback %', current_setting('work_mem'); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "after rollback 4MB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN COMMIT; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ; COMMIT AND CHAIN; RAISE NOTICE 'chained %', current_setting('transaction_isolation'); COMMIT; RAISE NOTICE 'plain %', current_setting('transaction_isolation'); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "chained repeatable read", ..N }, Diagnostic { code: "00000", message: "plain read committed", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN COMMIT; SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; ROLLBACK AND CHAIN; RAISE NOTICE 'chained %', current_setting('transaction_isolation'); ROLLBACK; RAISE NOTICE 'plain %', current_setting('transaction_isolation'); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "chained serializable", ..N }, Diagnostic { code: "00000", message: "plain read committed", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE t0 timestamptz := now(); BEGIN PERFORM pg_sleep(0.01); COMMIT; RAISE NOTICE 'new transaction %', now() > t0; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "new transaction t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN SELECT a FROM tt ORDER BY a LOOP INSERT INTO tt VALUES (r.a + 100); COMMIT; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                            &[T("101")],
                            &[T("103")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN UPDATE tt SET a = a WHERE a > 100 RETURNING a LOOP COMMIT; END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic {
                        code: "55000",
                        message: "cannot perform transaction commands inside a cursor loop that is not read-only",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tf_calls();",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL tp_commit();",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", ..E }),
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
                    query: "DO $$ BEGIN COMMIT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN BEGIN COMMIT; EXCEPTION WHEN OTHERS THEN RAISE; END; END $$;",
                    expected: Expected::Error(Diagnostic {
                        code: "2D000",
                        message: "cannot commit while a subtransaction is active",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tt;",
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
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tf_par() FROM tt;",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ALL;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET statement_timeout = '300ms';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN FOR i IN 1..10 LOOP COMMIT; PERFORM pg_sleep(0.1); END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET statement_timeout;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET transaction_timeout = '300ms';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN FOR i IN 1..10 LOOP PERFORM pg_sleep(0.1); COMMIT; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
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
            name: "a COMMIT inside a procedure is seen by other sessions at once and releases the procedure's locks",
            set_up_script: &[
                "CREATE TABLE vis (a INT);",
                "CREATE PROCEDURE p_vis() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO vis VALUES (1); LOCK TABLE vis IN ACCESS EXCLUSIVE MODE; COMMIT; PERFORM pg_sleep(1); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p_vis();",
                    client: "A",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vis;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE vis IN ACCESS EXCLUSIVE MODE NOWAIT;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM vis;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit inside a procedure commits the working set the procedure's COMMIT made",
            set_up_script: &[
                "CREATE TABLE dc (a INT PRIMARY KEY);",
                "CREATE PROCEDURE p_dc() LANGUAGE plpgsql AS $$ BEGIN INSERT INTO dc VALUES (1); COMMIT; PERFORM dolt_commit('-Am', 'from a procedure'); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL p_dc();",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("from a procedure")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a procedure that commits fails when a trigger calls it",
            set_up_script: &[
                "CREATE TABLE trt (a INT);",
                "CREATE PROCEDURE tp_only2() LANGUAGE plpgsql AS $$ BEGIN COMMIT; END $$;",
                "CREATE FUNCTION trg_calls() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN CALL tp_only2(); RETURN NEW; END $$;",
                "CREATE TRIGGER trt_bi BEFORE INSERT ON trt FOR EACH ROW EXECUTE FUNCTION trg_calls();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO trt VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "2D000", message: "invalid transaction termination", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_cached_call_follows_its_procedure_replaced() {
    run_scripts(&[
        ScriptTest {
            name: "a function's cached CALL depends on its procedure, so replacing the procedure plans the CALL again with the new default",
            set_up_script: &[
                "CREATE PROCEDURE qd_p(a int DEFAULT 1) LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'qd_p got %', a; END $$;",
                "CREATE FUNCTION qd_f() RETURNS void LANGUAGE plpgsql AS $$ BEGIN CALL qd_p(); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT qd_f();",
                    expected: Expected::Rows {
                        columns: &[Column("qd_f", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "qd_p got 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE qd_p(a int DEFAULT 2) LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'qd_p got %', a; END $$;",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT qd_f();",
                    expected: Expected::Rows {
                        columns: &[Column("qd_f", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "qd_p got 2", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_changes_a_rollback_undoes_are_gone_from_the_statement_s_later_commands() {
    run_scripts(&[
        ScriptTest {
            name: "catalog changes a subtransaction, a procedure's ROLLBACK, or ROLLBACK TO undoes are gone from later commands",
            set_up_script: &[
                "CREATE TABLE rt2 (a INT);",
                "CREATE PROCEDURE rp() LANGUAGE plpgsql AS $$ BEGIN CREATE TABLE rt4 (); ROLLBACK; CREATE TABLE rt5 (); PERFORM 1 FROM rt4; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN BEGIN CREATE TABLE rt1 (); RAISE EXCEPTION 'x'; EXCEPTION WHEN OTHERS THEN NULL; END; PERFORM 1 FROM rt1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "rt1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN BEGIN DROP TABLE rt2; RAISE EXCEPTION 'x'; EXCEPTION WHEN OTHERS THEN NULL; END; CREATE TABLE rt3 (); PERFORM 1 FROM rt2; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CALL rp();",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "rt4" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt6 ();",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT a;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt7 ();",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO a;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rt8 ();",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname LIKE 'rt%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("rt2")],
                            &[T("rt3")],
                            &[T("rt6")],
                            &[T("rt8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_a_cursor_loop_kept_past_a_commit_closes_when_an_error_ends_it() {
    run_scripts(&[
        ScriptTest {
            name: "a FOR loop's query kept past the loop's COMMIT is dropped when an error ends the block, leaving no cursor behind",
            set_up_script: &[
                "CREATE TABLE ploop_src (x INT);",
                "INSERT INTO ploop_src VALUES (0), (1), (2), (3);",
                "CREATE TABLE ploop_dst (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO LANGUAGE plpgsql $$ DECLARE r RECORD; BEGIN FOR r IN SELECT * FROM ploop_src ORDER BY x LOOP INSERT INTO ploop_dst (a) VALUES (12 / (r.x - 2)); COMMIT; END LOOP; END; $$;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM ploop_dst ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("-12")],
                            &[T("-6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
