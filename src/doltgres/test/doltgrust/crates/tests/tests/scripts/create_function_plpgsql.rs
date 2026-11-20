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
fn test_create_function_language_plpgsql() {
    run_scripts(&[
        ScriptTest {
            name: "ALIAS",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_alias(input TEXT)
				RETURNS TEXT AS $$
				DECLARE
					var1 TEXT;
					var2 TEXT;
				BEGIN
					DECLARE
						alias1 ALIAS FOR var1;
						alias2 ALIAS FOR alias1;
						alias3 ALIAS FOR input;
					BEGIN
						alias2 := alias3;
					END;
					RETURN var1;
				END;
				$$ LANGUAGE plpgsql;
				"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_alias('123');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_alias", TEXT)],
                        rows: &[
                            &[T("123")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Assignment",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_assignment(input TEXT) RETURNS TEXT AS $$
DECLARE
	var1 TEXT;
BEGIN
	var1 := 'Initial: ' || input;
	IF input = 'Hello' THEN
		var1 := var1 || ' - Greeting';
	ELSIF input = 'Bye' THEN
		var1 := var1 || ' - Farewell';
	ELSIF length(input) > 5 THEN
		var1 := var1 || ' - Over 5';
	ELSE
		var1 := var1 || ' - Else';
	END IF;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_assignment('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_assignment", TEXT)],
                        rows: &[
                            &[T("Initial: Hello - Greeting")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_assignment('Bye');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_assignment", TEXT)],
                        rows: &[
                            &[T("Initial: Bye - Farewell")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_assignment('abc');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_assignment", TEXT)],
                        rows: &[
                            &[T("Initial: abc - Else")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_assignment('something');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_assignment", TEXT)],
                        rows: &[
                            &[T("Initial: something - Over 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conditions that evaluate to NULL",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_null_if(input TEXT) RETURNS TEXT AS $$
BEGIN
	IF input = 'Hello' THEN
		RETURN 'Greeting';
	ELSIF input = 'Bye' THEN
		RETURN 'Farewell';
	ELSE
		RETURN 'Else';
	END IF;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_null_while(input TEXT) RETURNS INT AS $$
DECLARE
	count1 INT := 0;
BEGIN
	WHILE input = 'Hello' LOOP
		count1 := count1 + 1;
		EXIT WHEN count1 > 2;
	END LOOP;
	RETURN count1;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_null_exit(input TEXT) RETURNS INT AS $$
DECLARE
	count1 INT := 0;
BEGIN
	LOOP
		count1 := count1 + 1;
		EXIT WHEN input = 'Hello';
		EXIT WHEN count1 > 2;
	END LOOP;
	RETURN count1;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_null_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 1 THEN
			msg := 'one';
		ELSE
			msg := 'other';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_null_searched_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE
		WHEN x = 1 THEN
			msg := 'one';
		ELSE
			msg := 'other';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_if(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_if", TEXT)],
                        rows: &[
                            &[T("Else")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_if('Bye');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_if", TEXT)],
                        rows: &[
                            &[T("Farewell")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_while(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_while", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_while('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_while", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_exit(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_exit", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_exit('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_exit", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_case(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_case", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_case", TEXT)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_searched_case(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_searched_case", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null_searched_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null_searched_case", TEXT)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CASE, with ELSE",
            set_up_script: &[
                r#"
CREATE FUNCTION interpreted_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 1, 2 THEN
			msg := 'one';
			msg := msg || ' or two';
		ELSE
			msg := 'other';
			msg := msg || ' value than one or two';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("one or two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(2);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("one or two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(0);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("other value than one or two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CASE over a non-integer expression",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_case_text(x TEXT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 'Hello', 'Hi' THEN
			msg := 'greeting';
		WHEN 'Bye' THEN
			msg := 'farewell';
		ELSE
			msg := 'other';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_case_bool(x BOOLEAN) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN true THEN
			msg := 'yes';
		ELSE
			msg := 'no';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_case_numeric(x NUMERIC) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 1.5 THEN
			msg := 'one point five';
		ELSE
			msg := 'other';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_case_loop() RETURNS TEXT AS $$
DECLARE
	i INT := 0;
	msg TEXT;
	result TEXT := '';
BEGIN
	WHILE i < 3 LOOP
		i := i + 1;
		CASE i::TEXT
			WHEN '2' THEN
				msg := 'two';
			ELSE
				msg := 'n';
		END CASE;
		result := result || msg;
	END LOOP;
	RETURN result;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_text('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_text", TEXT)],
                        rows: &[
                            &[T("greeting")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_text('Bye');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_text", TEXT)],
                        rows: &[
                            &[T("farewell")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_text('zzz');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_text", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_bool(true);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_bool", TEXT)],
                        rows: &[
                            &[T("yes")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_bool(false);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_bool", TEXT)],
                        rows: &[
                            &[T("no")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_numeric(1.5);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_numeric", TEXT)],
                        rows: &[
                            &[T("one point five")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_numeric(2.5);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_numeric", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_loop();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_loop", TEXT)],
                        rows: &[
                            &[T("ntwon")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "NULL conditions are not met",
            set_up_script: &[
                "CREATE TABLE case_selector (v TEXT);",
                "INSERT INTO case_selector VALUES ('match');",
                r#"CREATE FUNCTION interpreted_case_null(x TEXT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 'match' THEN
			msg := 'matched';
		ELSE
			msg := 'fell through';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_case_empty_selector() RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE (SELECT v FROM case_selector LIMIT 1)
		WHEN 'match' THEN
			msg := 'matched';
		ELSE
			msg := 'fell through';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_if_null(x BOOLEAN) RETURNS TEXT AS $$
BEGIN
	IF x THEN
		RETURN 'true';
	ELSE
		RETURN 'not true';
	END IF;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_while_null(x BOOLEAN) RETURNS TEXT AS $$
BEGIN
	WHILE x LOOP
		RETURN 'looped';
	END LOOP;
	RETURN 'never looped';
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_null('match');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_null", TEXT)],
                        rows: &[
                            &[T("matched")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_null(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_null", TEXT)],
                        rows: &[
                            &[T("fell through")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_empty_selector();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_empty_selector", TEXT)],
                        rows: &[
                            &[T("matched")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM case_selector;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case_empty_selector();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case_empty_selector", TEXT)],
                        rows: &[
                            &[T("fell through")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_if_null(true);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_if_null", TEXT)],
                        rows: &[
                            &[T("true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_if_null(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_if_null", TEXT)],
                        rows: &[
                            &[T("not true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_while_null(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_while_null", TEXT)],
                        rows: &[
                            &[T("never looped")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CASE, without ELSE",
            set_up_script: &[
                r#"
CREATE FUNCTION interpreted_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE x
		WHEN 1, 2 THEN
			msg := 'one or two';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("one or two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(2);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("one or two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(0);",
                    expected: Expected::Error(Diagnostic { code: "20000", message: "case not found", hint: "CASE statement is missing ELSE part.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Searched CASE, with ELSE",
            set_up_script: &[
                r#"
CREATE FUNCTION interpreted_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE
		WHEN x BETWEEN 0 AND 10 THEN
			msg := 'value is between zero';
			msg := msg || ' and ten';
		WHEN x BETWEEN 11 AND 20 THEN
			msg := 'value is between eleven and twenty';
		ELSE
			msg := 'value';
			msg := msg || ' is';
			msg := msg || ' out of';
			msg := msg || ' bounds';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(0);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(10);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(11);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between eleven and twenty")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(21);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is out of bounds")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Searched CASE, without ELSE",
            set_up_script: &[
                r#"
CREATE FUNCTION interpreted_case(x INT) RETURNS TEXT AS $$
DECLARE
	msg TEXT;
BEGIN
	CASE
		WHEN x BETWEEN 0 AND 10 THEN
			msg := 'value is between zero and ten';
		WHEN x BETWEEN 11 AND 20 THEN
			msg := 'value';
			msg := msg || ' is between';
			msg := msg || ' eleven and';
			msg := msg || ' twenty';
	END CASE;
	RETURN msg;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(0);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(10);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between zero and ten")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(11);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_case", TEXT)],
                        rows: &[
                            &[T("value is between eleven and twenty")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_case(21);",
                    expected: Expected::Error(Diagnostic { code: "20000", message: "case not found", hint: "CASE statement is missing ELSE part.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CONTINUE",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_continue() RETURNS INT4 AS $$
DECLARE
	var1 INT4;
BEGIN
	LOOP
		var1 := var1 + 1;
		IF var1 < 4 THEN
			CONTINUE;
		END IF;
		RETURN var1;
	END LOOP;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_continue();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_continue", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CONTINUE Label",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_continue_label() RETURNS INT4 AS $$
DECLARE
	var1 INT4;
BEGIN
	<<cont_label>>
	LOOP
		var1 := var1 + 1;
		IF var1 < 6 THEN
			CONTINUE cont_label;
		END IF;
		RETURN var1;
	END LOOP;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_continue_label();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_continue_label", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: var1 was never initialized, so in Postgres the loop never ends.
        ScriptTest {
            name: "EXIT",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_exit() RETURNS INT4 AS $$
DECLARE
	var1 INT4 := 0;
BEGIN
	LOOP
		var1 := var1 + 1;
		IF var1 >= 8 THEN
			EXIT;
		END IF;
	END LOOP;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_exit();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_exit", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: var1 was never initialized, so in Postgres the loop never ends.
        ScriptTest {
            name: "EXIT WHEN",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_exit_when() RETURNS INT4 AS $$
DECLARE
	var1 INT4 := 0;
BEGIN
	LOOP
		var1 := var1 + 1;
		EXIT WHEN var1 >= 9;
	END LOOP;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_exit_when();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_exit_when", INT4)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: var1 was never initialized, so in Postgres NULL + 1 stays NULL and the loop never ends; Doltgres treated it as 0.
        ScriptTest {
            name: "LOOP",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_loop() RETURNS INT4 AS $$
DECLARE
	var1 INT4 := 0;
BEGIN
	LOOP
		var1 := var1 + 1;
		IF var1 >= 10 THEN
			RETURN var1;
		END IF;
	END LOOP;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_uninitialized() RETURNS INT4 AS $$
DECLARE
	var1 INT4;
BEGIN
	var1 := var1 + 1;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_loop();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_loop", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_uninitialized();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_uninitialized", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: var1 was never initialized, so in Postgres the loop never ends.
        ScriptTest {
            name: "LOOP Label",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_loop_label() RETURNS INT4 AS $$
DECLARE
	var1 INT4 := 0;
BEGIN
	<<loop_label>>
	LOOP
		var1 := var1 + 1;
		IF var1 >= 12 THEN
			EXIT loop_label;
		END IF;
	END LOOP;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_loop_label();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_loop_label", INT4)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PERFORM",
            set_up_script: &[
                "CREATE SEQUENCE test_sequence;",
                r#"CREATE FUNCTION interpreted_perform() RETURNS VOID AS $$
BEGIN
	PERFORM nextval('test_sequence');
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT nextval('test_sequence');",
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
                    query: "SELECT interpreted_perform();",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_perform", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('test_sequence');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF",
            set_up_script: &[
                r#"CREATE TYPE user_summary AS (
					user_id   integer,
					username  text,
					is_active boolean);"#,
                r#"CREATE OR REPLACE FUNCTION func2() RETURNS SETOF user_summary
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 1, 'username', true;
						RETURN QUERY SELECT 2, 'another', false;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username,t)")],
                            &[T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED), Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username,t)"), T("(1,username,t)")],
                            &[T("(2,another,f)"), T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF with no results",
            set_up_script: &[
                "CREATE TABLE user_summary (user_id integer, username text, is_active boolean);",
                r#"CREATE OR REPLACE FUNCTION func2() RETURNS SETOF user_summary
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT * from user_summary;
						RETURN QUERY SELECT * from user_summary;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED), Column("func2", USER_DEFINED)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF with type from other schema",
            set_up_script: &[
                "CREATE SCHEMA sch1;",
                r#"CREATE TYPE sch1.user_summary AS (
					user_id   integer,
					username  text,
					is_active boolean);"#,
                r#"CREATE OR REPLACE FUNCTION func2() RETURNS SETOF sch1.user_summary
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 1, 'username', true;
						RETURN QUERY SELECT 2, 'another', false;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username,t)")],
                            &[T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED), Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username,t)"), T("(1,username,t)")],
                            &[T("(2,another,f)"), T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF with param",
            set_up_script: &[
                r#"CREATE TYPE user_summary AS (
					user_id   integer,
					username  text,
					is_active boolean);"#,
                r#"CREATE OR REPLACE FUNCTION func3(user_id integer) RETURNS SETOF user_summary
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT user_id, 'username', true;
						RETURN QUERY SELECT user_id, 'another', false;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func3(111);",
                    expected: Expected::Rows {
                        columns: &[Column("func3", USER_DEFINED)],
                        rows: &[
                            &[T("(111,username,t)")],
                            &[T("(111,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func3(111), func3(222);",
                    expected: Expected::Rows {
                        columns: &[Column("func3", USER_DEFINED), Column("func3", USER_DEFINED)],
                        rows: &[
                            &[T("(111,username,t)"), T("(222,username,t)")],
                            &[T("(111,another,f)"), T("(222,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE",
            set_up_script: &[
                r#"CREATE FUNCTION func2() RETURNS TABLE(user_id integer, username  text, is_active boolean)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 1, 'username', true;
						RETURN QUERY SELECT 2, 'another', false;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", RECORD)],
                        rows: &[
                            &[T("(1,username,t)")],
                            &[T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", RECORD), Column("func2", RECORD)],
                        rows: &[
                            &[T("(1,username,t)"), T("(1,username,t)")],
                            &[T("(2,another,f)"), T("(2,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE with single field",
            set_up_script: &[
                r#"CREATE FUNCTION func2() RETURNS TABLE(username text)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 'username1';
						RETURN QUERY SELECT 'username2';
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", TEXT)],
                        rows: &[
                            &[T("username1")],
                            &[T("username2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", TEXT), Column("func2", TEXT)],
                        rows: &[
                            &[T("username1"), T("username1")],
                            &[T("username2"), T("username2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE with types from other schema",
            set_up_script: &[
                "CREATE SCHEMA sch1;",
                r#"CREATE TYPE sch1.mytype AS (
					user_id   integer,
					username  text);"#,
                r#"CREATE FUNCTION func2() RETURNS TABLE(foo sch1.mytype)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 1, 'username1';
						RETURN QUERY SELECT 2, 'username2';
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username1)")],
                            &[T("(2,username2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(), func2();",
                    expected: Expected::Rows {
                        columns: &[Column("func2", USER_DEFINED), Column("func2", USER_DEFINED)],
                        rows: &[
                            &[T("(1,username1)"), T("(1,username1)")],
                            &[T("(2,username2)"), T("(2,username2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: Postgres rejects an input parameter with the same name as an output column, so the input is renamed.
        ScriptTest {
            name: "RETURNS TABLE with param",
            set_up_script: &[
                r#"CREATE OR REPLACE FUNCTION func3(p_user_id integer) RETURNS TABLE(user_id integer, username  text, is_active boolean)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT p_user_id, 'username', true;
						RETURN QUERY SELECT p_user_id, 'another', false;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func3(111);",
                    expected: Expected::Rows {
                        columns: &[Column("func3", RECORD)],
                        rows: &[
                            &[T("(111,username,t)")],
                            &[T("(111,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func3(111), func3(222);",
                    expected: Expected::Rows {
                        columns: &[Column("func3", RECORD), Column("func3", RECORD)],
                        rows: &[
                            &[T("(111,username,t)"), T("(222,username,t)")],
                            &[T("(111,another,f)"), T("(222,another,f)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE with join query",
            set_up_script: &[
                r#"CREATE TABLE customers (
					id INT PRIMARY KEY,
					name TEXT
				);"#,
                r#"CREATE TABLE orders (
					id SERIAL PRIMARY KEY,
					customer_id INT,
					amount INT
				);"#,
                "INSERT INTO customers VALUES (1, 'John'), (2, 'Jane');",
                "INSERT INTO orders VALUES (1, 1, 100), (2, 2, 10);",
                r#"CREATE OR REPLACE FUNCTION func2(n INT) RETURNS TABLE (c_id INT, c_name TEXT, c_total_spent INT) 
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY
						SELECT c.id,
							   c.name,
							   SUM(o.amount) AS total_spent
						FROM customers c
						JOIN orders o ON o.customer_id = c.id
						GROUP BY c.id, c.name
						HAVING SUM(o.amount) >= n
						;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func2(1);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: r#"Returned type bigint does not match expected type integer in column "c_total_spent" (position 3)."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(11);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: r#"Returned type bigint does not match expected type integer in column "c_total_spent" (position 3)."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func2(111);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: r#"Returned type bigint does not match expected type integer in column "c_total_spent" (position 3)."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE with conflicting variable names",
            set_up_script: &[
                r#"CREATE TABLE customers2 (
					id INT PRIMARY KEY,
					name TEXT
				);"#,
                "INSERT INTO customers2 VALUES (1, 'John'), (2, 'Jane');",
                r#"CREATE OR REPLACE FUNCTION func_conflict(n INT) RETURNS TABLE (id INT, name TEXT) 
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY
						SELECT c.id, c.name
						FROM customers2 c
						WHERE c.id = n;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func_conflict(1);",
                    expected: Expected::Rows {
                        columns: &[Column("func_conflict", RECORD)],
                        rows: &[
                            &[T("(1,John)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func_conflict(2);",
                    expected: Expected::Rows {
                        columns: &[Column("func_conflict", RECORD)],
                        rows: &[
                            &[T("(2,Jane)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF with composite param",
            set_up_script: &[
                r#"CREATE TYPE user_summary AS (
					user_id   integer,
					username  text,
					is_active boolean);"#,
                r#"CREATE OR REPLACE FUNCTION func3(u user_summary) RETURNS SETOF user_summary
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT u.user_id, u.username, u.is_active;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT func3((222,'passedin',false)::user_summary);",
                    expected: Expected::Rows {
                        columns: &[Column("func3", USER_DEFINED)],
                        rows: &[
                            &[T("(222,passedin,f)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RAISE",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_raise1(input TEXT) RETURNS TEXT AS $$
				DECLARE
					var1 TEXT;
				BEGIN
					RAISE WARNING 'MyMessage';
					RAISE NOTICE USING MESSAGE = 'MyNoticeMessage';
					RAISE DEBUG 'DebugTest1' USING MESSAGE = 'DebugMessage';
					var1 := input;
					RETURN var1;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_raise2(input TEXT) RETURNS TEXT AS $$
				DECLARE
					var1 TEXT;
				BEGIN
					RAISE EXCEPTION '% %% bar %', 'foo', 1+1;
					var1 := input;
					RETURN var1;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_raise_errcode() RETURNS TEXT AS $$
				BEGIN
					RAISE EXCEPTION 'coded' USING ERRCODE = '22012';
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_raise_condition_name() RETURNS TEXT AS $$
				BEGIN
					RAISE EXCEPTION 'named' USING ERRCODE = 'division_by_zero';
				END;
				$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_raise1('123');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "RAISE option already specified: MESSAGE", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "MyMessage", ..E }, Diagnostic { code: "00000", message: "MyNoticeMessage", ..N }],
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_raise2('123');",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "foo % bar 2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_raise_errcode();",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "coded", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_raise_condition_name();",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "named", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT INTO",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_select_into(input INT4) RETURNS TEXT AS $$
DECLARE
	ret TEXT;
	count INT4;
BEGIN
	DROP TABLE IF EXISTS temp_table;
	CREATE TABLE temp_table (pk SERIAL PRIMARY KEY, v1 TEXT NOT NULL);
	INSERT INTO temp_table (v1) VALUES ('abc'), ('def'), ('ghi');
	SELECT COUNT(*) INTO count FROM temp_table;
	IF input > 0 AND input <= count THEN
		SELECT v1 INTO ret FROM temp_table WHERE pk = input;
	ELSE
		ret := 'out of bounds';
	END IF;
	RETURN ret;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_select_into(1);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_select_into", TEXT)],
                        rows: &[
                            &[T("abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: r#"table "temp_table" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_select_into(2);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_select_into", TEXT)],
                        rows: &[
                            &[T("def")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_select_into(3);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_select_into", TEXT)],
                        rows: &[
                            &[T("ghi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_select_into(4);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_select_into", TEXT)],
                        rows: &[
                            &[T("out of bounds")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHILE",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_while(input INT4) RETURNS INT AS $$
DECLARE
	counter INT4;
BEGIN
	WHILE counter + input < 100 LOOP
		-- Include more than one statement in the loop so it's not too simple 
		counter = counter + 1;
		counter = counter - 1;
		counter = counter + 1;
	END LOOP;
	RETURN counter;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_while(42);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_while", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHILE Label",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_while_label(input INT4) RETURNS INT AS $$
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
	RETURN input;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_while_label(42);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_while_label", INT4)],
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
            name: "NULL",
            set_up_script: &[
                r#"CREATE FUNCTION interpreted_null(input INT) RETURNS TEXT AS $$
BEGIN
	IF input = 42 THEN
		NULL;
		NULL;
	ELSE
		RETURN 'No'; 
	END IF;
	NULL;
	RETURN 'Yes'; 
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_null(42);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null", TEXT)],
                        rows: &[
                            &[T("Yes")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_null(43);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_null", TEXT)],
                        rows: &[
                            &[T("No")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DO statement",
            set_up_script: &[
                "CREATE TABLE interpreted_do_values (v INT PRIMARY KEY)",
                r#"CREATE FUNCTION interpreted_do() RETURNS void AS $function$
BEGIN
	DO $block$ BEGIN
		INSERT INTO interpreted_do_values VALUES (42);
	END $block$;
END;
$function$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_do()",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_do", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM interpreted_do_values",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DO statement failure is atomic",
            set_up_script: &[
                "CREATE TABLE interpreted_do_atomic (v INT PRIMARY KEY)",
                r#"CREATE FUNCTION interpreted_do_error() RETURNS void AS $function$
BEGIN
	DO $block$ BEGIN
		INSERT INTO interpreted_do_atomic VALUES (1);
		RAISE EXCEPTION 'nested DO failed';
	END $block$;
END;
$function$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_do_error()",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "nested DO failed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM interpreted_do_atomic",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Variable reference substitution",
            set_up_script: &[
                r#"
CREATE FUNCTION test1(input TEXT) RETURNS TEXT AS $$
DECLARE
	var1 TEXT;
BEGIN
	var1 := 'input' || input;
	IF var1 = 'input' || input THEN
		RETURN var1 || 'var1';
	ELSE
		RETURN '!!!';
	END IF;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT test1('Hello');",
                    expected: Expected::Rows {
                        columns: &[Column("test1", TEXT)],
                        rows: &[
                            &[T("inputHellovar1")],
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
                r#"CREATE FUNCTION interpreted_overload(input TEXT) RETURNS TEXT AS $$
DECLARE
	var1 TEXT;
BEGIN
	IF length(input) > 3 THEN
		var1 := input || '_long';
	ELSE
		var1 := input;
	END IF;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION interpreted_overload(input INT4) RETURNS INT4 AS $$
DECLARE
	var1 INT4;
BEGIN
	IF input > 3 THEN
		var1 := -input;
	ELSE
		var1 := input;
	END IF;
	RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT interpreted_overload('abc');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_overload", TEXT)],
                        rows: &[
                            &[T("abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_overload('abcd');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_overload", TEXT)],
                        rows: &[
                            &[T("abcd_long")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_overload(3);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_overload", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_overload(4);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_overload", INT4)],
                        rows: &[
                            &[T("-4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Branching",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION interpreted_as_of(input TEXT) RETURNS TEXT AS $$
BEGIN
	RETURN input || '_extra';
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_as_of('abcd');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_as_of", TEXT)],
                        rows: &[
                            &[T("abcd_extra")],
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
                    query: r#"CREATE OR REPLACE FUNCTION interpreted_as_of(input TEXT) RETURNS TEXT AS $$
BEGIN
	RETURN input;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
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
                    query: "SELECT interpreted_as_of('abc');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_as_of", TEXT)],
                        rows: &[
                            &[T("abc")],
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
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT interpreted_as_of('abcd');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_as_of", TEXT)],
                        rows: &[
                            &[T("abcd_extra")],
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
                "CREATE TABLE test(pk INT4);",
                "INSERT INTO test VALUES (77);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION interpreted_merging(input TEXT) RETURNS TEXT AS $$
BEGIN
	RETURN input || '_extra';
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging('abcd');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_merging", TEXT)],
                        rows: &[
                            &[T("abcd_extra")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging(55);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function interpreted_merging(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
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
                    query: r#"CREATE FUNCTION interpreted_merging(input INT4) RETURNS INT4 AS $$
BEGIN
	RETURN input + 11;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
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
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging(55);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_merging", INT4)],
                        rows: &[
                            &[T("66")],
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
                    query: "INSERT INTO test VALUES (80);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging('abcde');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_merging", TEXT)],
                        rows: &[
                            &[T("abcde_extra")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging(67);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function interpreted_merging(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("80")],
                            &[T("77")],
                        ],
                        tag: "SELECT 2",
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
                    query: "SELECT interpreted_merging('abcdef');",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_merging", TEXT)],
                        rows: &[
                            &[T("abcdef_extra")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT interpreted_merging(58);",
                    expected: Expected::Rows {
                        columns: &[Column("interpreted_merging", INT4)],
                        rows: &[
                            &[T("69")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("80")],
                            &[T("77")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSERT values from function",
            set_up_script: &[
                "CREATE TABLE test (v1 TEXT);",
                r#"CREATE FUNCTION insertion_text() RETURNS TEXT AS $$
DECLARE
    var1 TEXT;
BEGIN
    var1 := 'example';
    RETURN var1;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (insertion_text()), (insertion_text());",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", TEXT)],
                        rows: &[
                            &[T("example")],
                            &[T("example")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create function on different branch",
            set_up_script: &[
                r#"CREATE FUNCTION f1(input TEXT) RETURNS TEXT AS $$
BEGIN
	RETURN input || '_extra';
END;
$$ LANGUAGE plpgsql;"#,
                "call dolt_branch('b1');",
                r#"CREATE FUNCTION "postgres/b1".public.f1(input INT4) RETURNS INT4 AS $$
BEGIN
	RETURN input + 11;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            skip: Some("Doltgres' setup CALLs Dolt's functions, which Doltgrust and Dolt run with SELECT, so neither Postgres nor the Go server recorded this script; re-port it with SELECT and record it"),
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT f1('abcd');",
                    flow: Flow::Query,
                    skip: Some("no expectation was recorded: Doltgres' setup CALLs Dolt's functions, which neither Postgres nor the Go server ran; re-port it with SELECT and record it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "postgres/b1".public.f1(55);"#,
                    flow: Flow::Query,
                    skip: Some("no expectation was recorded: Doltgres' setup CALLs Dolt's functions, which neither Postgres nor the Go server ran; re-port it with SELECT and record it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "call dolt_checkout('b1');",
                    flow: Flow::Query,
                    skip: Some("no expectation was recorded: Doltgres' setup CALLs Dolt's functions, which neither Postgres nor the Go server ran; re-port it with SELECT and record it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1(55);",
                    flow: Flow::Query,
                    skip: Some("no expectation was recorded: Doltgres' setup CALLs Dolt's functions, which neither Postgres nor the Go server ran; re-port it with SELECT and record it"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Nested IF statements with exceptions",
            set_up_script: &[
                "CREATE TABLE public.table_name (start_date DATE NOT NULL, end_date DATE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.fn_name() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
BEGIN
    IF NEW.start_date IS NOT NULL
       AND NEW.end_date IS NULL
    THEN
        NEW.end_date := NEW.start_date + INTERVAL '31 day';
    END IF;
    IF NEW.start_date IS NOT NULL
       AND NEW.end_date IS NOT NULL
    THEN
        IF NEW.end_date < NEW.start_date THEN
            RAISE EXCEPTION 'end_date (%) start_date (%)',
                NEW.end_date, NEW.start_date;
        END IF;
        IF NEW.end_date > (NEW.start_date + INTERVAL '31 day') THEN
            RAISE EXCEPTION 'Too far (start_date=%, end_date=%)',
                NEW.start_date, NEW.end_date;
        END IF;
    END IF;
    RETURN NEW;
END;
$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trig_name BEFORE INSERT OR UPDATE ON public.table_name FOR EACH ROW EXECUTE FUNCTION public.fn_name();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.table_name VALUES ('2025-01-02', '2025-02-02');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.table_name VALUES ('2025-04-05', NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.table_name VALUES ('2025-09-10', '2025-07-08');",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "end_date (2025-07-08) start_date (2025-09-10)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.table_name VALUES ('2025-11-11', '2025-12-31');",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "Too far (start_date=2025-11-11, end_date=2025-12-31)", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table as type for functions",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, name TEXT NOT NULL, qty INT4 NOT NULL, price REAL NOT NULL);",
                "INSERT INTO test VALUES (1, 'apple', 3, 2.5), (2, 'banana', 5, 1.2);",
                "CREATE FUNCTION total(t test) RETURNS REAL AS $$ BEGIN RETURN t.qty * t.price; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION priceHike(t test, pricehike REAL) RETURNS test AS $$ BEGIN RETURN (t.id, t.name, t.qty, t.price + pricehike)::test; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION singleReturn() RETURNS test AS $$ DECLARE result test; BEGIN SELECT * INTO result FROM test WHERE id = 1; RETURN result; END; $$ LANGUAGE plpgsql;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT total(t) FROM test AS t;",
                    expected: Expected::Rows {
                        columns: &[Column("total", FLOAT4)],
                        rows: &[
                            &[T("7.5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT priceHike(t, 10.0) FROM test AS t;",
                    expected: Expected::Rows {
                        columns: &[Column("pricehike", USER_DEFINED)],
                        rows: &[
                            &[T("(1,apple,3,12.5)")],
                            &[T("(2,banana,5,11.2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT priceHike(ROW(3, 'orange', 1, 1.8)::test, 100.0);",
                    expected: Expected::Rows {
                        columns: &[Column("pricehike", USER_DEFINED)],
                        rows: &[
                            &[T("(3,orange,1,101.8)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT singleReturn();",
                    expected: Expected::Rows {
                        columns: &[Column("singlereturn", USER_DEFINED)],
                        rows: &[
                            &[T("(1,apple,3,2.5)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table as type for columns",
            set_up_script: &[
                "CREATE TABLE t1 (v1 INT4 PRIMARY KEY, v2 TEXT NOT NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (v1 INT4 PRIMARY KEY, v2 t1 NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1, ROW(0, 'hello')::t1), (2, ROW(10, 'world')::t1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(0,hello)")],
                            &[T("2"), T("(10,world)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AlexTransit_venderctl import dump",
            set_up_script: &[
                r#"CREATE TYPE public.tax_job_state AS ENUM (
    'sched',
    'busy',
    'final',
    'help'
);"#,
                r#"CREATE TABLE public.catalog (
    vmid integer NOT NULL,
    code text NOT NULL,
    name text NOT NULL
);"#,
                r#"CREATE SEQUENCE public.tax_job_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;"#,
                r#"CREATE TABLE public.tax_job (
    id bigint NOT NULL,
    state public.tax_job_state NOT NULL,
    created timestamp with time zone NOT NULL,
    modified timestamp with time zone NOT NULL,
    scheduled timestamp with time zone,
    worker text,
    processor text,
    ext_id text,
    data jsonb,
    gross integer,
    notes text[],
    ops jsonb
);"#,
                r#"CREATE TABLE public.trans (
    vmid integer NOT NULL,
    vmtime timestamp with time zone,
    received timestamp with time zone NOT NULL,
    menu_code text NOT NULL,
    options integer[],
    price integer NOT NULL,
    method integer NOT NULL,
    tax_job_id bigint,
    executer bigint,
    exeputer_type integer,
    executer_str text
);"#,
                "ALTER TABLE ONLY public.tax_job ALTER COLUMN id SET DEFAULT nextval('public.tax_job_id_seq'::regclass);",
                "INSERT INTO public.trans VALUES (1, '2023-04-05 06:07:08', '2023-05-06 07:08:09', 'test', ARRAY[5,7], 44, 1, NULL, 1, 1, '');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.tax_job_trans(t public.trans) RETURNS public.tax_job
    LANGUAGE plpgsql
    AS '
    # print_strict_params ON
DECLARE
    tjd jsonb;
    ops jsonb;
    tj tax_job;
    name text;
BEGIN
    -- lock trans row
    PERFORM
        1
    FROM
        trans
    WHERE (vmid, vmtime) = (t.vmid,
        t.vmtime)
LIMIT 1
FOR UPDATE;
    -- if trans already has tax_job assigned, just return it
    IF t.tax_job_id IS NOT NULL THEN
        SELECT
            * INTO STRICT tj
        FROM
            tax_job
        WHERE
            id = t.tax_job_id;
        RETURN tj;
    END IF;
    -- op code to human friendly name via catalog
    SELECT
        catalog.name INTO name
    FROM
        catalog
    WHERE (vmid, code) = (t.vmid,
        t.menu_code);
    IF NOT found THEN
        name := ''#'' || t.menu_code;
    END IF;
    ops := jsonb_build_array (jsonb_build_object(''vmid'', t.vmid, ''time'', t.vmtime, ''name'', name, ''code'', t.menu_code, ''amount'', 1, ''price'', t.price, ''method'', t.method));
    INSERT INTO tax_job (state, created, modified, scheduled, processor, ops, gross)
        VALUES (''sched'', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, ''ru2019'', ops, t.price)
    RETURNING
        * INTO STRICT tj;
    UPDATE
        trans
    SET
        tax_job_id = tj.id
    WHERE (vmid, vmtime) = (t.vmid,
        t.vmtime);
    RETURN tj;
END;
';"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.tax_job_trans(trans.*) FROM public.trans;",
                    expected: Expected::Rows {
                        columns: &[Column("tax_job_trans", USER_DEFINED)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolve type with empty search path",
            set_up_script: &[
                "set search_path to ''",
                "CREATE TABLE public.ambienttempdetail (tempdetailid integer NOT NULL, panelprojectid integer, threshold_value numeric(10,2), readingintervalinmin integer);",
                "insert into public.ambienttempdetail values (1, 101, 25.5, 15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.ambienttempdetail_insertupdate(p_panel_project_id integer, p_threshold_value numeric, p_reading_interval_in_min integer) RETURNS integer
    LANGUAGE plpgsql
    AS $$
DECLARE
    v_rtn_value INTEGER;
BEGIN
    IF NOT EXISTS (SELECT * FROM AmbientTempDetail WHERE PanelProjectId = p_panel_project_id) THEN
        INSERT INTO AmbientTempDetail (PanelProjectId, Threshold_Value, ReadingIntervalInMin)
        VALUES (p_panel_project_id, p_threshold_value, p_reading_interval_in_min)
        RETURNING TempDetailId INTO v_rtn_value;
    ELSE
        UPDATE AmbientTempDetail
        SET PanelProjectId = p_panel_project_id,
            Threshold_Value = p_threshold_value,
            ReadingIntervalInMin = p_reading_interval_in_min
        WHERE PanelProjectId = p_panel_project_id;
        v_rtn_value := p_panel_project_id;
    END IF;
    
    RETURN v_rtn_value;
END;
$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "set search_path to 'public'",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.ambienttempdetail_insertupdate(101, 25.5, 15);",
                    expected: Expected::Rows {
                        columns: &[Column("ambienttempdetail_insertupdate", INT4)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create function on non-existent table that does not exist yet with 'check_function_bodies'",
            assertions: &[
                ScriptTestAssertion {
                    query: "SHOW check_function_bodies;",
                    expected: Expected::Rows {
                        columns: &[Column("check_function_bodies", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.film_in_stock(p_film_id integer, p_store_id integer, OUT p_film_count integer) RETURNS SETOF integer
    LANGUAGE sql
    AS $_$
     SELECT inventory_id
     FROM inventory
     WHERE film_id = $1
     AND store_id = $2
     AND inventory_in_stock(inventory_id);
$_$;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "inventory" does not exist"#, position: 188, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION public.film_in_stock(p_film_id integer, p_store_id integer, OUT p_film_count integer) RETURNS SETOF integer
    LANGUAGE sql
    AS $_$
     SELECT inventory_id
     FROM inventory
     WHERE film_id = $1
     AND store_id = $2
     AND inventory_in_stock(inventory_id);
$_$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE public.inventory (
    inventory_id integer DEFAULT nextval('public.inventory_inventory_id_seq'::regclass) NOT NULL,
    film_id smallint NOT NULL,
    store_id smallint NOT NULL,
    last_update timestamp without time zone DEFAULT now() NOT NULL
);
"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "public.inventory_inventory_id_seq" does not exist"#, position: 74, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DECLARE variable with default value of literal value or parameter reference",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION d() RETURNS TEXT[] AS $$ DECLARE chars TEXT[] := '{A,B,C,D,E,F,G,H}'; BEGIN RETURN chars; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d();",
                    expected: Expected::Rows {
                        columns: &[Column("d", TEXT_ARRAY)],
                        rows: &[
                            &[T("{A,B,C,D,E,F,G,H}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION
  mylt2 (x text, y text, e int) RETURNS boolean LANGUAGE plpgsql AS $$
declare
  xx text COLLATE "POSIX" := x;
  yy text := y;
  zz int := e;
begin
  return xx < yy;
end
$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mylt2('a', 'B', 1) as f;",
                    expected: Expected::Rows {
                        columns: &[Column("f", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DECLARE variable with default value of an expression",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION array_default() RETURNS TEXT[] AS $$ DECLARE permitted TEXT[] := ARRAY['retired_at', 'deleted_at']; BEGIN RETURN permitted; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_default();",
                    expected: Expected::Rows {
                        columns: &[Column("array_default", TEXT_ARRAY)],
                        rows: &[
                            &[T("{retired_at,deleted_at}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION quote_default() RETURNS TEXT AS $$ DECLARE x TEXT := 'it''s'; BEGIN RETURN x; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT quote_default();",
                    expected: Expected::Rows {
                        columns: &[Column("quote_default", TEXT)],
                        rows: &[
                            &[T("it's")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION call_default() RETURNS TEXT AS $$ DECLARE x TEXT := upper('abc') || length('abcd'); BEGIN RETURN x; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT call_default();",
                    expected: Expected::Rows {
                        columns: &[Column("call_default", TEXT)],
                        rows: &[
                            &[T("ABC4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION chained_default(p INT) RETURNS TEXT AS $$
DECLARE
	a INT := p * 2;
	b INT := a + 1;
	c TEXT := 'a=' || a || ' b=' || b;
	d INT := (SELECT count(*) FROM (VALUES (1), (2)) v);
BEGIN
	RETURN c || ' d=' || d;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT chained_default(5);",
                    expected: Expected::Rows {
                        columns: &[Column("chained_default", TEXT)],
                        rows: &[
                            &[T("a=10 b=11 d=2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION cast_default() RETURNS TEXT AS $$
DECLARE
	a NUMERIC := 1.5::numeric + 1;
	b TEXT := NULL;
	c INT[] := ARRAY[1, 2, 3];
	d TIMESTAMP := '2020-01-01 00:00:00'::timestamp;
BEGIN
	RETURN a || '|' || coalesce(b, 'nil') || '|' || c[2] || '|' || d;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cast_default();",
                    expected: Expected::Rows {
                        columns: &[Column("cast_default", TEXT)],
                        rows: &[
                            &[T("2.5|nil|2|2020-01-01 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION qualified_default() RETURNS TEXT AS $$
DECLARE
	k CONSTANT TEXT := upper('abc');
	n TEXT NOT NULL := repeat('n', 2);
BEGIN
	RETURN k || n;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT qualified_default();",
                    expected: Expected::Rows {
                        columns: &[Column("qualified_default", TEXT)],
                        rows: &[
                            &[T("ABCnn")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR I LOOP statement",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION code()
RETURNS VARCHAR AS $$
DECLARE
chars TEXT[] := '{A,B,C,D,E,F,G,H}';
  result TEXT := '';
  i INTEGER;
BEGIN
FOR i IN 1..3 LOOP
    result := result || chars[1+i];
END LOOP;
RETURN result;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT code();",
                    expected: Expected::Rows {
                        columns: &[Column("code", VARCHAR)],
                        rows: &[
                            &[T("BCD")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR S LOOP statement",
            set_up_script: &[
                r#"CREATE TABLE decks (
    id bigint NOT NULL,
    name text DEFAULT NULL::character varying,
    parent bigint
);"#,
                "INSERT INTO decks VALUES (1, 'name1', 2), (2, 'name2', 4), (5, 'name3', 1), (7, 'name4', 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION delete_deck_tree(p_id bigint) RETURNS void
            LANGUAGE plpgsql
            AS $$
        DECLARE
           r record;
        BEGIN
           DELETE FROM decks WHERE parent = p_id;
        
           FOR r IN SELECT id FROM decks WHERE parent = p_id LOOP
              PERFORM delete_deck_tree(r.id);
           END LOOP;
        
           DELETE FROM decks WHERE id = p_id;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * from decks;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("name", TEXT), Column("parent", INT8)],
                        rows: &[
                            &[T("1"), T("name1"), T("2")],
                            &[T("2"), T("name2"), T("4")],
                            &[T("5"), T("name3"), T("1")],
                            &[T("7"), T("name4"), T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT delete_deck_tree(1);",
                    expected: Expected::Rows {
                        columns: &[Column("delete_deck_tree", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * from decks;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("name", TEXT), Column("parent", INT8)],
                        rows: &[
                            &[T("2"), T("name2"), T("4")],
                            &[T("7"), T("name4"), T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION fors_nested_one_var() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            r RECORD;
            result TEXT := '';
        BEGIN
            FOR r IN SELECT 1 AS n UNION ALL SELECT 2 LOOP FOR r IN SELECT 8 AS n UNION ALL SELECT 9 LOOP result := result || r.n; END LOOP; END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fors_nested_one_var();",
                    expected: Expected::Rows {
                        columns: &[Column("fors_nested_one_var", TEXT)],
                        rows: &[
                            &[T("8989")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOREACH IN ARRAY statement",
            set_up_script: &[
                "CREATE TABLE tags (id int PRIMARY KEY, vals text[]);",
                "INSERT INTO tags VALUES (1, '{a,b}'), (2, '{c}');",
                "CREATE TABLE maybe_tags (id int PRIMARY KEY, vals text[]);",
                "INSERT INTO maybe_tags VALUES (1, '{a,b}'), (3, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION concat_all(arr TEXT[]) RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
            result TEXT := '';
        BEGIN
            FOREACH col IN ARRAY arr LOOP
                result := result || col;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT concat_all('{a,b,c}');",
                    expected: Expected::Rows {
                        columns: &[Column("concat_all", TEXT)],
                        rows: &[
                            &[T("abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT concat_all('{}'), concat_all('{a,NULL,b}');",
                    expected: Expected::Rows {
                        columns: &[Column("concat_all", TEXT), Column("concat_all", TEXT)],
                        rows: &[
                            &[T(""), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, concat_all(vals) FROM tags ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("concat_all", TEXT)],
                        rows: &[
                            &[T("1"), T("ab")],
                            &[T("2"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION sum_halves() RETURNS NUMERIC
            LANGUAGE plpgsql
            AS $$
        DECLARE
            n NUMERIC;
            total NUMERIC := 0;
        BEGIN
            FOREACH n IN ARRAY ARRAY[1, 2, 3] LOOP
                total := total + n / 2;
            END LOOP;
            RETURN total;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum_halves();",
                    expected: Expected::Rows {
                        columns: &[Column("sum_halves", NUMERIC)],
                        rows: &[
                            &[T("3.00000000000000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_found(arr TEXT[]) RETURNS BOOLEAN
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
        BEGIN
            FOREACH col IN ARRAY arr LOOP
            END LOOP;
            RETURN FOUND;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_found('{a}'), foreach_found('{}');",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_found", BOOL), Column("foreach_found", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_exit(arr TEXT[]) RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
            result TEXT := '';
        BEGIN
            FOREACH col IN ARRAY arr LOOP
                CONTINUE WHEN col = 'skip';
                EXIT WHEN col = 'stop';
                result := result || col;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_exit('{a,skip,b,stop,c}');",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_exit", TEXT)],
                        rows: &[
                            &[T("ab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_labeled() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            r RECORD;
            col TEXT;
            result TEXT := '';
        BEGIN
            <<rows>>
            FOR r IN SELECT id, vals FROM tags ORDER BY id LOOP
                FOREACH col IN ARRAY r.vals LOOP
                    EXIT rows WHEN col = 'c';
                    result := result || r.id || col;
                END LOOP;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_labeled();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_labeled", TEXT)],
                        rows: &[
                            &[T("1a1b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_nested() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            a TEXT;
            b TEXT;
            result TEXT := '';
        BEGIN
            FOREACH a IN ARRAY ARRAY['1', '2'] LOOP
                FOREACH b IN ARRAY ARRAY['x', 'y'] LOOP
                    result := result || a || b;
                END LOOP;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_nested();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_nested", TEXT)],
                        rows: &[
                            &[T("1x1y2x2y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_nested_one_line() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            a TEXT;
            b TEXT;
            result TEXT := '';
        BEGIN
            FOREACH a IN ARRAY ARRAY['1', '2'] LOOP FOREACH b IN ARRAY ARRAY['x', 'y'] LOOP result := result || a || b; END LOOP; END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_nested_one_line();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_nested_one_line", TEXT)],
                        rows: &[
                            &[T("1x1y2x2y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_nested_one_var() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            x TEXT;
            result TEXT := '';
        BEGIN
            FOREACH x IN ARRAY ARRAY['1', '2'] LOOP FOREACH x IN ARRAY ARRAY['a', 'b'] LOOP result := result || x; END LOOP; END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_nested_one_var();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_nested_one_var", TEXT)],
                        rows: &[
                            &[T("abab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_quoted() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            "MyCol" TEXT;
            result TEXT := '';
        BEGIN
            FOREACH "MyCol" IN ARRAY ARRAY['x', 'y'] LOOP
                result := result || "MyCol";
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_quoted();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_quoted", TEXT)],
                        rows: &[
                            &[T("xy")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_query() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
            result TEXT := '';
        BEGIN
            FOREACH col IN ARRAY (SELECT vals FROM tags WHERE id = 1) LOOP
                DELETE FROM tags WHERE id = 1;
                result := result || col;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_query();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_query", TEXT)],
                        rows: &[
                            &[T("ab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_null() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
        BEGIN
            FOREACH col IN ARRAY CAST(NULL AS TEXT[]) LOOP
            END LOOP;
            RETURN 'no raise';
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_null();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_null_column(tag_id INT) RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            arr TEXT[];
            col TEXT;
            result TEXT := '';
        BEGIN
            SELECT vals INTO arr FROM maybe_tags WHERE id = tag_id;
            FOREACH col IN ARRAY arr LOOP
                result := result || col;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_null_column(1);",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_null_column", TEXT)],
                        rows: &[
                            &[T("ab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_null_column(3);",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_undefaulted() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            arr TEXT[];
            col TEXT;
            result TEXT := 'none';
        BEGIN
            FOREACH col IN ARRAY arr LOOP
                result := 'ran';
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_undefaulted();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_not_array() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
        BEGIN
            FOREACH col IN ARRAY 42 LOOP
            END LOOP;
            RETURN 'no raise';
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_not_array();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "FOREACH expression must yield an array, not type integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_bare_null() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            col TEXT;
        BEGIN
            FOREACH col IN ARRAY NULL LOOP
            END LOOP;
            RETURN 'no raise';
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_bare_null();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_composite() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            r RECORD;
            result TEXT := '';
        BEGIN
            FOREACH r IN ARRAY (SELECT array_agg(maybe_tags) FROM maybe_tags) LOOP
                result := result || r.id;
            END LOOP;
            RETURN result;
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_composite();",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION foreach_slice() RETURNS TEXT
            LANGUAGE plpgsql
            AS $$
        DECLARE
            row1 TEXT[];
        BEGIN
            FOREACH row1 SLICE 1 IN ARRAY ARRAY[['a', 'b']] LOOP
            END LOOP;
            RETURN 'done';
        END;
        $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT foreach_slice();",
                    expected: Expected::Rows {
                        columns: &[Column("foreach_slice", TEXT)],
                        rows: &[
                            &[T("done")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "use multiple types returning in block statement",
            set_up_script: &[
                "CREATE TABLE test (id int, v text);",
                "INSERT INTO test VALUES (1, 'r'), (2, 'g');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION return_table() RETURNS int AS $$ 
DECLARE ti int; tt text; 
BEGIN 
	INSERT INTO test VALUES (3, 'w') returning * INTO ti, tt; 
	RETURN ti; 
END; 
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select return_table();",
                    expected: Expected::Rows {
                        columns: &[Column("return_table", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "distinct expression over function",
            set_up_script: &[
                "CREATE TABLE test (pk SERIAL PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION func1() RETURNS INT4 AS $$
DECLARE
  ret INT4;
BEGIN
  INSERT INTO test VALUES (DEFAULT) RETURNING pk INTO ret;
  IF ret % 2 <> 0 THEN
    RETURN NULL;
  END IF;
  RETURN ret;
END; $$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT func1() IS DISTINCT FROM func1() as DISTINCT_RESULT;",
                    expected: Expected::Rows {
                        columns: &[Column("distinct_result", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "function with single OUT parameter",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION calculate_bonus(
    IN current_salary NUMERIC,
    OUT new_total_salary NUMERIC
) AS $$
BEGIN
    -- Calculate the new total with a 10% bonus
    new_total_salary := current_salary + current_salary * 0.10;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("calculate_bonus", NUMERIC)],
                        rows: &[
                            &[T("5500.00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("new_total_salary", NUMERIC)],
                        rows: &[
                            &[T("5500.00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT new_total_salary FROM calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("new_total_salary", NUMERIC)],
                        rows: &[
                            &[T("5500.00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "function with multiple OUT parameter",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION calculate_bonus(
    IN current_salary NUMERIC,
    OUT bonus_amount NUMERIC,
    OUT new_total_salary NUMERIC
) AS $$
BEGIN
    -- Calculate a 10% bonus
    bonus_amount := current_salary * 0.10;
    
    -- Calculate the new total
    new_total_salary := current_salary + bonus_amount;
END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("calculate_bonus", RECORD)],
                        rows: &[
                            &[T("(500.00,5500.00)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("bonus_amount", NUMERIC), Column("new_total_salary", NUMERIC)],
                        rows: &[
                            &[T("500.00"), T("5500.00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bonus_amount FROM calculate_bonus(5000);",
                    expected: Expected::Rows {
                        columns: &[Column("bonus_amount", NUMERIC)],
                        rows: &[
                            &[T("500.00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS TABLE in a FROM clause",
            set_up_script: &[
                r#"CREATE FUNCTION figures() RETURNS TABLE(shape TEXT, sides INT)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 'triangle', 3;
						RETURN QUERY SELECT 'square', 4;
					END;
					$$;"#,
            ],
            assertions: &[
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
            name: "RETURNS TABLE with a single column in a FROM clause",
            set_up_script: &[
                r#"CREATE FUNCTION shapes() RETURNS TABLE(shape TEXT)
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 'triangle';
						RETURN QUERY SELECT 'square';
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM shapes();",
                    expected: Expected::Rows {
                        columns: &[Column("shape", TEXT)],
                        rows: &[
                            &[T("triangle")],
                            &[T("square")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shapes();",
                    expected: Expected::Rows {
                        columns: &[Column("shapes", TEXT)],
                        rows: &[
                            &[T("triangle")],
                            &[T("square")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RETURNS SETOF a scalar type",
            set_up_script: &[
                r#"CREATE FUNCTION sides() RETURNS SETOF INT
					LANGUAGE plpgsql
					AS $$
					BEGIN
						RETURN QUERY SELECT 3 UNION ALL SELECT 4;
					END;
					$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM sides();",
                    expected: Expected::Rows {
                        columns: &[Column("sides", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sides();",
                    expected: Expected::Rows {
                        columns: &[Column("sides", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
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
fn test_plpgsql_rules() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL control flow",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION classify(n int) RETURNS text LANGUAGE plpgsql AS $$ BEGIN IF n < 0 THEN RETURN 'negative'; ELSIF n = 0 THEN RETURN 'zero'; ELSE RETURN 'positive'; END IF; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classify(-5), classify(0), classify(7);",
                    expected: Expected::Rows {
                        columns: &[Column("classify", TEXT), Column("classify", TEXT), Column("classify", TEXT)],
                        rows: &[
                            &[T("negative"), T("zero"), T("positive")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION case_of(n int) RETURNS text LANGUAGE plpgsql AS $$ BEGIN CASE n WHEN 1, 2 THEN RETURN 'small'; WHEN 3 THEN RETURN 'three'; END CASE; RETURN 'unreachable'; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT case_of(2), case_of(3);",
                    expected: Expected::Rows {
                        columns: &[Column("case_of", TEXT), Column("case_of", TEXT)],
                        rows: &[
                            &[T("small"), T("three")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT case_of(9);",
                    expected: Expected::Error(Diagnostic { code: "20000", message: "case not found", hint: "CASE statement is missing ELSE part.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION searched_case(n int) RETURNS text LANGUAGE plpgsql AS $$ BEGIN CASE WHEN n > 10 THEN RETURN 'big'; ELSE RETURN 'little'; END CASE; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT searched_case(11), searched_case(1);",
                    expected: Expected::Rows {
                        columns: &[Column("searched_case", TEXT), Column("searched_case", TEXT)],
                        rows: &[
                            &[T("big"), T("little")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION loops(n int) RETURNS text LANGUAGE plpgsql AS $$ DECLARE out text := ''; i int := 0; BEGIN <<outer>> LOOP i := i + 1; EXIT outer WHEN i > n; CONTINUE WHEN i = 2; out := out || i; END LOOP; WHILE i > 0 LOOP i := i - 2; out := out || '-'; END LOOP; FOR j IN REVERSE 10..1 BY 4 LOOP out := out || j; END LOOP; RETURN out; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loops(4);",
                    expected: Expected::Rows {
                        columns: &[Column("loops", TEXT)],
                        rows: &[
                            &[T("134---1062")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION query_loop() RETURNS text LANGUAGE plpgsql AS $$ DECLARE r record; out text := ''; BEGIN FOR r IN SELECT * FROM (VALUES (1, 'a'), (2, 'b')) v(n, s) ORDER BY n LOOP out := out || r.n || r.s; END LOOP; RETURN out; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT query_loop();",
                    expected: Expected::Rows {
                        columns: &[Column("query_loop", TEXT)],
                        rows: &[
                            &[T("1a2b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION array_loop(a int[]) RETURNS int LANGUAGE plpgsql AS $$ DECLARE x int; total int := 0; BEGIN FOREACH x IN ARRAY a LOOP total := total + x; END LOOP; RETURN total; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_loop(ARRAY[1, 2, 3]);",
                    expected: Expected::Rows {
                        columns: &[Column("array_loop", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_loop(NULL);",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION exit_outside() RETURNS int LANGUAGE plpgsql AS $$ BEGIN EXIT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "EXIT cannot be used outside a loop, unless it has a label", position: 73, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION no_return(n int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN n := n + 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT no_return(1);",
                    expected: Expected::Error(Diagnostic { code: "2F005", message: "control reached end of function without RETURN", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL statements and FOUND",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE items (id int PRIMARY KEY, name text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lookup(want int) RETURNS text LANGUAGE plpgsql AS $$ DECLARE result text; BEGIN SELECT name INTO result FROM items WHERE id = want; IF NOT FOUND THEN RETURN 'missing'; END IF; RETURN result; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lookup(1), lookup(3);",
                    expected: Expected::Rows {
                        columns: &[Column("lookup", TEXT), Column("lookup", TEXT)],
                        rows: &[
                            &[T("one"), T("missing")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION strict_lookup(want int) RETURNS text LANGUAGE plpgsql AS $$ DECLARE result text; BEGIN SELECT name INTO STRICT result FROM items WHERE id = want OR want = 0; RETURN result; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strict_lookup(2);",
                    expected: Expected::Rows {
                        columns: &[Column("strict_lookup", TEXT)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strict_lookup(3);",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strict_lookup(0);",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bump() RETURNS boolean LANGUAGE plpgsql AS $$ BEGIN UPDATE items SET name = name || '!' WHERE id = 1; RETURN FOUND; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bump();",
                    expected: Expected::Rows {
                        columns: &[Column("bump", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM items WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("one!")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION discard() RETURNS int LANGUAGE plpgsql AS $$ BEGIN SELECT 1; RETURN 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT discard();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query has no destination for result data", hint: "If you want to discard the results of a SELECT, use PERFORM instead.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION performs() RETURNS boolean LANGUAGE plpgsql AS $$ BEGIN PERFORM 1 FROM items WHERE id = 99; RETURN FOUND; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT performs();",
                    expected: Expected::Rows {
                        columns: &[Column("performs", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dynamic(t text, want int) RETURNS text LANGUAGE plpgsql AS $$ DECLARE result text; BEGIN EXECUTE 'SELECT name FROM ' || t || ' WHERE id = $1' INTO result USING want; RETURN result; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dynamic('items', 2);",
                    expected: Expected::Rows {
                        columns: &[Column("dynamic", TEXT)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION null_dynamic() RETURNS void LANGUAGE plpgsql AS $$ BEGIN EXECUTE NULL; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT null_dynamic();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "query string argument of EXECUTE is null", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION all_items() RETURNS SETOF items LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT * FROM items ORDER BY id; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION wrong_shape() RETURNS TABLE(a int, b int) LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 1, 'x'::text; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wrong_shape();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: r#"Returned type text does not match expected type integer in column "b" (position 2)."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION too_few() RETURNS TABLE(a int, b int) LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM too_few();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: "Number of returned columns (1) does not match expected column count (2).", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pairs() RETURNS TABLE(a int, b text) LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT id, name FROM items ORDER BY id; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pairs();",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one!")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION out_params(x int, OUT doubled int, OUT label text) LANGUAGE plpgsql AS $$ BEGIN doubled := x * 2; label := 'n' || x; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT out_params(4);",
                    expected: Expected::Rows {
                        columns: &[Column("out_params", RECORD)],
                        rows: &[
                            &[T("(8,n4)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM out_params(5);",
                    expected: Expected::Rows {
                        columns: &[Column("doubled", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("10"), T("n5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION void_result() RETURNS void LANGUAGE plpgsql AS $$ BEGIN END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT void_result(), void_result() IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("void_result", VOID), Column("?column?", BOOL)],
                        rows: &[
                            &[T(""), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL RAISE",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION notices(n int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'n is %, doubled %', n, n * 2; RAISE WARNING 'careful'; RAISE INFO 'info %%'; RAISE DEBUG 'hidden'; RETURN n; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT notices(3);",
                    expected: Expected::Rows {
                        columns: &[Column("notices", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "n is 3, doubled 6", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "careful", ..E }, Diagnostic { severity: "INFO", code: "00000", message: "info %", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fails(n int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'bad value: %', n USING HINT = 'try ' || (n + 1), DETAIL = 'detail here', ERRCODE = '22023'; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fails(1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "bad value: 1", detail: "detail here", hint: "try 2", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fails_default() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'plain'; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fails_default();",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "plain", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fails_named() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'dup' USING ERRCODE = 'unique_violation'; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fails_named();",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "dup", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION null_param() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'value: %', NULL::int; RETURN 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT null_param();",
                    expected: Expected::Rows {
                        columns: &[Column("null_param", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "value: <NULL>", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION too_many() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'x', 1; RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "too many parameters specified for RAISE", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL variables and declarations",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION defaults(p int) RETURNS text LANGUAGE plpgsql AS $$ DECLARE a int := p * 2; b int := a + 1; c text := 'a=' || a || ' b=' || b; BEGIN RETURN c; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT defaults(5);",
                    expected: Expected::Rows {
                        columns: &[Column("defaults", TEXT)],
                        rows: &[
                            &[T("a=10 b=11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION aliases(input text) RETURNS text LANGUAGE plpgsql AS $$ DECLARE v text; BEGIN DECLARE a1 ALIAS FOR v; a2 ALIAS FOR input; BEGIN a1 := a2 || '?'; END; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT aliases('x');",
                    expected: Expected::Rows {
                        columns: &[Column("aliases", TEXT)],
                        rows: &[
                            &[T("x?")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION positional(int, int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN $1 * $2; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT positional(6, 7);",
                    expected: Expected::Rows {
                        columns: &[Column("positional", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad_type() RETURNS int LANGUAGE plpgsql AS $$ DECLARE b pg_catalog.integer; BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "pg_catalog.integer" does not exist"#, position: 73, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad_syntax() RETURNS int LANGUAGE plpgsql AS $$ DECLARE b pg_catalog.double precision; BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "precision""#, position: 93, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION missing_type() RETURNS int LANGUAGE plpgsql AS $$ DECLARE b no_such_type; BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "no_such_type" does not exist"#, position: 77, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE things (id int, label text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO things VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION row_types() RETURNS text LANGUAGE plpgsql AS $$ DECLARE r things; l things.label%TYPE; BEGIN SELECT * INTO r FROM things; l := r.label || '!'; RETURN r.id || l; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_types();",
                    expected: Expected::Rows {
                        columns: &[Column("row_types", TEXT)],
                        rows: &[
                            &[T("1a!")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION records() RETURNS text LANGUAGE plpgsql AS $$ DECLARE r record := ROW(1, 'a'); BEGIN SELECT 2 AS x, 'b' AS y INTO r; RETURN r.x || r.y; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT records();",
                    expected: Expected::Rows {
                        columns: &[Column("records", TEXT)],
                        rows: &[
                            &[T("2b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION unassigned() RETURNS int LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN RETURN r.x; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unassigned();",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"record "r" is not assigned yet"#, detail: "The tuple structure of a not-yet-assigned record is indeterminate.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_binding_rules() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL variables and column names",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tjv (id serial, ops jsonb, gross int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION p1() RETURNS int LANGUAGE plpgsql AS $$ DECLARE ops jsonb; BEGIN ops := '[1]'; INSERT INTO tjv (ops, gross) VALUES (ops, 5); RETURN 1; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p1();",
                    expected: Expected::Rows {
                        columns: &[Column("p1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION p2() RETURNS int LANGUAGE plpgsql AS $$ DECLARE gross int := 9; BEGIN UPDATE tjv SET gross = gross; RETURN 1; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p2();",
                    expected: Expected::Error(Diagnostic { code: "42702", message: r#"column reference "gross" is ambiguous"#, detail: "It could refer to either a PL/pgSQL variable or a table column.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ops, gross FROM tjv;",
                    expected: Expected::Rows {
                        columns: &[Column("ops", JSONB), Column("gross", INT4)],
                        rows: &[
                            &[T("[1]"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE transv (vmid integer, price int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO transv VALUES (1, 44);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION n1(t public.transv) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN t.price; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n1(transv.*) FROM transv;",
                    expected: Expected::Rows {
                        columns: &[Column("n1", INT4)],
                        rows: &[
                            &[T("44")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Unassigned records and RAISE options",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION u1() RETURNS text LANGUAGE plpgsql AS $$ DECLARE r RECORD; BEGIN RETURN to_jsonb(r)::text; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT u1() IS NULL;",
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
                    query: "CREATE FUNCTION u3() RETURNS bool LANGUAGE plpgsql AS $$ DECLARE r RECORD; BEGIN RETURN r IS NULL; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT u3();",
                    expected: Expected::Rows {
                        columns: &[Column("u3", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r1() RETURNS text LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'one' USING MESSAGE = 'two'; RETURN 'x'; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r1();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "RAISE option already specified: MESSAGE", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Function bodies, concatenation, and system columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION later_table() RETURNS SETOF int LANGUAGE sql AS $$ SELECT id FROM not_created_yet $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = true;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a'::text || '2020-01-01'::timestamp, '2020-01-01'::timestamp || 'a'::text;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("a2020-01-01 00:00:00"), T("2020-01-01 00:00:00a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE st (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO st VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid = 'st'::regclass, cmin, xmax FROM st;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("cmin", CID), Column("xmax", XID)],
                        rows: &[
                            &[T("t"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.tableoid::regclass, n.tableoid::regclass FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid WHERE c.relname = 'st';",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("tableoid", REGCLASS)],
                        rows: &[
                            &[T("pg_class"), T("pg_namespace")],
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
fn test_positional_parameters() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL positional parameter references",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION positional_out(a INT, b TEXT, OUT c INT, OUT d TEXT) AS $$ BEGIN $3 := $1 + 1; d := $2 || 'x'; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM positional_out(5, 'q');",
                    expected: Expected::Rows {
                        columns: &[Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("6"), T("qx")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION positional_unnamed(INT, TEXT) RETURNS TEXT AS $$ BEGIN RETURN $2 || $1; END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT positional_unnamed(7, 'n');",
                    expected: Expected::Rows {
                        columns: &[Column("positional_unnamed", TEXT)],
                        rows: &[
                            &[T("n7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION positional_range(i ANYRANGE) RETURNS ANYMULTIRANGE AS $$ BEGIN RETURN multirange($1); END; $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT positional_range(int4range(1, 4)), pg_typeof(positional_range(int4range(1, 4)));",
                    expected: Expected::Rows {
                        columns: &[Column("positional_range", INT4MULTIRANGE), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("{[1,4)}"), T("int4multirange")],
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
fn test_return_next_and_dynamic_loops() {
    run_scripts(&[
        ScriptTest {
            name: "RETURN NEXT and FOR over EXECUTE",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE rn (id INT, name TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rn VALUES (1, 'a'), (2, 'b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION f(t text) RETURNS SETOF int LANGUAGE plpgsql AS $$
DECLARE r record;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
DECLARE r record;""#, position: 65, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN
  FOR r IN EXECUTE 'SELECT x * $1 AS y FROM generate_series(1, 3) x' USING 2 LOOP
    RETURN NEXT r.y;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 9, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  END LOOP;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "LOOP""#, position: 7, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"  FOR r IN EXECUTE format('SELECT %L::text AS v', t) LOOP
    RAISE NOTICE 'got %', r.v;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 3, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  END LOOP;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "LOOP""#, position: 7, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM f('hi');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function f(unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION g(n int, OUT a int, OUT b text) RETURNS SETOF record LANGUAGE plpgsql AS $$
BEGIN
  FOR i IN 1..n LOOP
    a := i; b := 'x' || i;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
BEGIN
  FOR i IN 1..n LOOP
    a := i; b := 'x' || i;""#, position: 90, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "    RETURN NEXT;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "RETURN""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  END LOOP;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "LOOP""#, position: 7, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g(3);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function g(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION h() RETURNS SETOF rn LANGUAGE plpgsql AS $$
DECLARE r rn;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
DECLARE r rn;""#, position: 58, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN
  FOR r IN SELECT * FROM rn ORDER BY id LOOP
    RETURN NEXT r;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 9, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  END LOOP;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "LOOP""#, position: 7, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  RETURN;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "RETURN""#, position: 3, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM h();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function h() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION k() RETURNS TABLE (x int, y text) LANGUAGE plpgsql AS $$
BEGIN
  x := 1; y := 'one'; RETURN NEXT;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
BEGIN
  x := 1; y := 'one'; RETURN NEXT;""#, position: 71, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  RETURN QUERY SELECT 2, 'two';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "RETURN""#, position: 3, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  x := 3; y := 'three'; RETURN NEXT;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "x""#, position: 3, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM k();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function k() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION fl() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE j json; a int; b text;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
DECLARE j json; a int; b text;""#, position: 61, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN
  FOR j IN EXECUTE 'SELECT ''{"x":1}''::json' LOOP RETURN NEXT j->>'x'; END LOOP;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 9, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "  FOR a, b IN SELECT 1, 'one' UNION ALL SELECT 2, 'two' LOOP RETURN NEXT a || b; END LOOP;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 3, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fl();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function fl() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION h5() RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE r record;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$
DECLARE r record;""#, position: 63, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN
  FOR r IN SELECT * FROM rn ORDER BY id LOOP RETURN NEXT r; END LOOP;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "FOR""#, position: 9, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unterminated dollar-quoted string at or near "$$;""#, position: 5, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM h5() AS t(a int, b text);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function h5() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column definition lists",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION q() RETURNS SETOF record LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 1, 'a'::text; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q() AS t(a int, b text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION q2() RETURNS SETOF record LANGUAGE sql AS $$ SELECT 1, 'a'::text $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM q2() AS (a int, b text);",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 2) AS g(x int);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"a column definition list is only allowed for functions returning "record""#, position: 42, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "storage parameters and planner statistics",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ro (a INT) WITH (fillfactor = 50, autovacuum_enabled = false, toast.autovacuum_enabled = false);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ro2 (a INT) WITH (bogus = 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized parameter "bogus""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ro4 (a INT) WITH (toast.fillfactor = 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized parameter "fillfactor""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ro5 (a INT) WITH (oids = false);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ro6 (a INT) WITH (oids = true);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "tables declared WITH OIDS are not supported", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ro SET (parallel_workers = 2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ro RESET (fillfactor);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ro SET (nope = 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized parameter "nope""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pg_class SET reltuples = 1000 WHERE relname = 'ro';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pg_class SET reltuples = 2, relpages = 5 WHERE relname = 'nothing';",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_validator() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE FUNCTION checks a PL/pgSQL function's body as Postgres' validator does",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, n INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f1(a int) RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN a + 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f2() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN 1 END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at end of input", position: 76, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f3() RETURNS int LANGUAGE plpgsql AS $$
DECLARE
  x int := 1;
  x text;
BEGIN
  RETURN x;
END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"duplicate declaration at or near "x""#, position: 81, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f4() RETURNS void LANGUAGE plpgsql AS $$ BEGIN RETURN 5; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "RETURN cannot have a parameter in function returning void", position: 71, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f5() RETURNS int LANGUAGE plpgsql AS $$ BEGIN SELEC 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "SELEC""#, position: 63, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f6() RETURNS int LANGUAGE plpgsql AS $$ BEGIN x := 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#""x" is not a known variable"#, position: 63, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f7() RETURNS int LANGUAGE plpgsql AS $$ DECLARE c CONSTANT int := 1; BEGIN c := 2; RETURN c; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22005", message: r#"variable "c" is declared CONSTANT"#, position: 92, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f8() RETURNS int LANGUAGE plpgsql AS $$ BEGIN EXIT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "EXIT cannot be used outside a loop, unless it has a label", position: 63, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f9() RETURNS int LANGUAGE plpgsql AS $$ <<a>> BEGIN RETURN 1; END b $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"end label "b" differs from block's label "a""#, position: 83, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f10() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.x := 1; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f11(int) RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "trigger functions cannot have declared arguments", hint: "The arguments of the trigger can be accessed through TG_NARGS and TG_ARGV instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f12() RETURNS int LANGUAGE plpgsql AS $$ DECLARE v nosuchtype; BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "nosuchtype" does not exist"#, position: 68, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f13() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'a % %', 1; RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "too few parameters specified for RAISE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f14() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE bogus_cond; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized exception condition "bogus_cond""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f15() RETURNS int LANGUAGE plpgsql AS $$ BEGIN IF true THEN RETURN 1; END IF; RETURN (1 + ); END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ")""#, position: 107, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f16() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "RETURN cannot have a parameter in function returning set", hint: "Use RETURN NEXT or RETURN QUERY.", position: 77, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f17() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot use RETURN NEXT in a non-SETOF function", position: 64, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f18() RETURNS int LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT 1 LOOP END LOOP; FOR i IN 1..10 BY 2 LOOP CONTINUE WHEN i > 3; END LOOP; RETURN 1; EXCEPTION WHEN division_by_zero OR SQLSTATE '22001' THEN RETURN 0; WHEN others THEN RETURN -1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f19() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN 1; EXCEPTION WHEN nosuch THEN RETURN 0; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized exception condition "nosuch""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f20(x int) RETURNS int LANGUAGE plpgsql AS $$ DECLARE y x%TYPE; z pg_class%ROWTYPE; BEGIN GET DIAGNOSTICS y = ROW_COUNT; RETURN y; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f21() RETURNS int LANGUAGE plpgsql AS $$ BEGIN GET STACKED DIAGNOSTICS x = ROW_COUNT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#""x" is not a known variable"#, position: 88, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN RAISE NOTICE 'hi' END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "END""#, position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f22() RETURNS int LANGUAGE plpgsql AS $$ DECLARE x int NOT NULL; BEGIN RETURN 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: r#"variable "x" must have a default value, since it's declared NOT NULL"#, position: 72, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f23() RETURNS int LANGUAGE plpgsql AS $$ BEGIN CASE 1 WHEN 1 THEN RETURN 1; ELSE RETURN 2; END CASE; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f24() RETURNS int LANGUAGE plpgsql AS $$ DECLARE c CURSOR (a int) FOR SELECT a; v int; BEGIN OPEN c(1); FETCH c INTO v; CLOSE c; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fa() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN RETURN 1 + 1 $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at end of input", position: 77, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fb() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN RETURN (1 END END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "mismatched parentheses at end of input", position: 82, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fc() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN x int $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "unexpected end of function definition at end of input", position: 70, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fd() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN RAISE NOTICE $q$a$q$ END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "END""#, position: 85, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fe() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN RETURN 1 END END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "END""#, position: 77, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ff() RETURNS int LANGUAGE plpgsql AS $f$
BEGIN
  INSERT INTO nosuch VALUES (1) RETURNING a INTO STRICT b;
END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#""b" is not a known variable"#, position: 120, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fg(a int, b text, OUT c int, OUT d text) LANGUAGE plpgsql AS $f$ BEGIN c := a; d := b; RETURN; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fh(a anyelement) RETURNS anyelement LANGUAGE plpgsql AS $f$ BEGIN RETURN $0; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fi(a int, a int) RETURNS int LANGUAGE plpgsql AS $f$ BEGIN RETURN 1; END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: r#"parameter name "a" used more than once"#, position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fj() RETURNS int LANGUAGE plpgsql AS $f$ DECLARE r record; BEGIN SELECT 1 INTO r, x; RETURN 1; END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "record variable cannot be part of multiple-item INTO list", position: 97, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fk() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN FOR i IN REVERSE SELECT 1 LOOP END LOOP; END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot specify REVERSE in query FOR loop", position: 73, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fl() RETURNS int LANGUAGE plpgsql AS $f$ #variable_conflict use_column
BEGIN RETURN 1; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fm() RETURNS int LANGUAGE plpgsql AS $f$ BEGIN <<lbl>> LOOP EXIT lbl; END LOOP; CONTINUE nosuch; END $f$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"there is no label "nosuch" attached to any block or loop enclosing this statement"#, position: 106, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fn() RETURNS int LANGUAGE plpgsql AS $f$ DECLARE v int; BEGIN EXECUTE 'select 1' INTO v USING 1, 2; FOREACH v IN ARRAY ARRAY[1] LOOP END LOOP; ASSERT v > 0, 'x'; RETURN v; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_settings() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL's settings: placeholders set before it loads become its settings when it loads",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "plpgsql.variable_conflict""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = 'use_column';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("use_column")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'bogus';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL plpgsql.print_strict_params = 'maybe';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.check_asserts = OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = 'all';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "22023", message: r#"parameter "plpgsql.print_strict_params" requires a Boolean value"#, ..E }, Diagnostic { severity: "WARNING", code: "22023", message: r#"invalid value for parameter "plpgsql.extra_warnings": "bogus""#, detail: r#"Unrecognized key word: "bogus"."#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.print_strict_params;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.print_strict_params", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.check_asserts;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.check_asserts", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("all")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("use_column")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.print_strict_params;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.print_strict_params", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.check_asserts;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.check_asserts", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = 'nope';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.variable_conflict": "nope""#, hint: "Available values: error, use_variable, use_column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'shadowed_variables, all';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.extra_warnings": "shadowed_variables, all""#, detail: r#"Key word "all" cannot be combined with other key words."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'shadowed_variables,too_many_rows';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("shadowed_variables,too_many_rows")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'foo';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.extra_warnings": "foo""#, detail: r#"Unrecognized key word: "foo"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'none,too_many_rows';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.extra_warnings": "none,too_many_rows""#, detail: r#"Key word "none" cannot be combined with other key words."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.bogus = 1;",
                    expected: Expected::Error(Diagnostic { code: "42602", message: r#"invalid configuration parameter name "plpgsql.bogus""#, detail: r#""plpgsql" is a reserved prefix."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.bogus;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "plpgsql.bogus""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plpgsql.variable_conflict;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("error")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ALL;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL's settings: reserving its prefix removes other placeholders, and its settings check their values",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plpgsql.foo = 'x';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = shadowed_variables, too_many_rows;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "SET plpgsql.extra_errors takes only one argument", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.check_asserts = 'off';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = 'USE_COLUMN';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("USE_COLUMN")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42602", message: r#"invalid configuration parameter name "plpgsql.foo", removing it"#, detail: r#""plpgsql" is now a reserved prefix."#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("use_column")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = shadowed_variables, too_many_rows;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("shadowed_variables, too_many_rows")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = ' Shadowed_Variables ';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T(" Shadowed_Variables ")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = 'ALL';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("ALL")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = '';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = 'a b';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.extra_errors": "a b""#, detail: "List syntax is invalid.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET plpgsql.extra_errors = '"too_many_rows"';"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T(r#""too_many_rows""#)],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.foo;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "plpgsql.foo""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.check_asserts = 1.5;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"parameter "plpgsql.check_asserts" requires a Boolean value"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = 1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "plpgsql.variable_conflict": "1""#, hint: "Available values: error, use_variable, use_column.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL's settings: values reapplied when it loads keep their transaction levels",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = use_variable;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'too_many_rows';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT a;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL plpgsql.check_asserts = OFF;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.print_strict_params = ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE a;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.check_asserts;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.check_asserts", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.print_strict_params;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.print_strict_params", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("use_variable")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.check_asserts;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.check_asserts", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.print_strict_params;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.print_strict_params", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_proc WHERE proname = 'f';",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'shadowed_variables';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL plpgsql.extra_errors = 'too_many_rows';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("too_many_rows")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("shadowed_variables")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_errors;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_errors", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ALL;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.extra_warnings;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.extra_warnings", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("error")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL's settings: a placeholder a role may not set is dropped with a warning when PL/pgSQL loads",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE gucu;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO gucu;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE gucu;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.variable_conflict = 'use_column';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.print_strict_params = ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42501", message: r#"permission denied to set parameter "plpgsql.variable_conflict""#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.variable_conflict;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.variable_conflict", TEXT)],
                        rows: &[
                            &[T("error")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW plpgsql.print_strict_params;",
                    expected: Expected::Rows {
                        columns: &[Column("plpgsql.print_strict_params", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
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
            name: "PL/pgSQL's settings: pg_settings lists them once PL/pgSQL loads",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plpgsql.print_strict_params = ON;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, setting, context, vartype, source, enumvals FROM pg_settings WHERE name LIKE 'plpgsql%' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT), Column("context", TEXT), Column("vartype", TEXT), Column("source", TEXT), Column("enumvals", TEXT_ARRAY)],
                        rows: &[
                            &[T("plpgsql.check_asserts"), T("on"), T("user"), T("bool"), T("default"), Null],
                            &[T("plpgsql.extra_errors"), T("none"), T("user"), T("string"), T("default"), Null],
                            &[T("plpgsql.extra_warnings"), T("none"), T("user"), T("string"), T("default"), Null],
                            &[T("plpgsql.print_strict_params"), T("on"), T("user"), T("bool"), T("session"), Null],
                            &[T("plpgsql.variable_conflict"), T("error"), T("superuser"), T("enum"), T("default"), T("{error,use_variable,use_column}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_compile_warnings() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL compile warnings: extra checks warn or fail with their positions in the statement",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'shadowed_variables';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s1(x INT) RETURNS INT LANGUAGE plpgsql AS $$DECLARE x INT; BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42712", message: r#"variable "x" shadows a previously defined variable"#, position: 69, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = 'all';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s2(x INT) RETURNS INT LANGUAGE plpgsql AS $$DECLARE x INT; BEGIN RETURN 1; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42712", message: r#"variable "x" shadows a previously defined variable"#, position: 69, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plpgsql.extra_errors;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'none';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s3(x INT) RETURNS INT LANGUAGE plpgsql AS $$DECLARE x INT; BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'shadowed_variables';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; BEGIN DECLARE x INT; BEGIN END; END$$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s4(x INT) RETURNS INT LANGUAGE plpgsql AS 'DECLARE x INT; BEGIN RETURN 1; END';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42712", message: r#"variable "x" shadows a previously defined variable"#, position: 68, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION s5(x INT) RETURNS INT LANGUAGE plpgsql AS E'DECLARE\n x INT; BEGIN RETURN 1; END';"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42712", message: r#"variable "x" shadows a previously defined variable"#, ..E }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL compile warnings: the lexer's warnings come before the statement checks' warnings and errors",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET standard_conforming_strings = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION w() RETURNS INT LANGUAGE plpgsql AS $$BEGIN PERFORM 'a\\b'; RETURN 1; END$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "22P06", message: r#"nonstandard use of \\ in a string literal"#, hint: r#"Use the escape string syntax for backslashes, e.g., E'\\'."#, position: 69, ..E }, Diagnostic { severity: "WARNING", code: "22P06", message: r#"nonstandard use of \\ in a string literal"#, hint: r#"Use the escape string syntax for backslashes, e.g., E'\\'."#, position: 69, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION w2() RETURNS INT LANGUAGE plpgsql AS $$BEGIN
  IF 'a\\b' = 'x' THEN RETURN 2; END IF; RETURN 1; END$$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "22P06", message: r#"nonstandard use of \\ in a string literal"#, hint: r#"Use the escape string syntax for backslashes, e.g., E'\\'."#, position: 67, ..E }, Diagnostic { severity: "WARNING", code: "22P06", message: r#"nonstandard use of \\ in a string literal"#, hint: r#"Use the escape string syntax for backslashes, e.g., E'\\'."#, position: 67, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION w3() RETURNS INT LANGUAGE plpgsql AS $$BEGIN PERFORM 1 FROM nosuch WHERE 'a\\b' = (; RETURN 1; END$$;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"mismatched parentheses at or near ";""#, position: 100, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "22P06", message: r#"nonstandard use of \\ in a string literal"#, hint: r#"Use the escape string syntax for backslashes, e.g., E'\\'."#, position: 90, ..E }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_execution() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL execution: expressions, loops, IF, CASE, and RETURN",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION add1(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN x + 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT add1(41);",
                    expected: Expected::Rows {
                        columns: &[Column("add1", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fact(n INT) RETURNS BIGINT LANGUAGE plpgsql AS $$
DECLARE r BIGINT := 1;
BEGIN
  FOR i IN 1..n LOOP r := r * i; END LOOP;
  RETURN r;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fact(10);",
                    expected: Expected::Rows {
                        columns: &[Column("fact", INT8)],
                        rows: &[
                            &[T("3628800")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION classify(x INT) RETURNS TEXT LANGUAGE plpgsql AS $$
BEGIN
  IF x < 0 THEN RETURN 'neg'; ELSIF x = 0 THEN RETURN 'zero'; END IF;
  CASE x WHEN 1 THEN RETURN 'one'; ELSE RETURN 'many'; END CASE;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classify(-5), classify(0), classify(1), classify(7);",
                    expected: Expected::Rows {
                        columns: &[Column("classify", TEXT), Column("classify", TEXT), Column("classify", TEXT), Column("classify", TEXT)],
                        rows: &[
                            &[T("neg"), T("zero"), T("one"), T("many")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION w(n INT) RETURNS INT LANGUAGE plpgsql AS $$
DECLARE i INT := 0; s INT := 0;
BEGIN
  WHILE i < n LOOP i := i + 1; CONTINUE WHEN i % 2 = 0; s := s + i; EXIT WHEN s > 20; END LOOP;
  RETURN s;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT w(5), w(100);",
                    expected: Expected::Rows {
                        columns: &[Column("w", INT4), Column("w", INT4)],
                        rows: &[
                            &[T("9"), T("25")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION lbl() RETURNS INT LANGUAGE plpgsql AS $$
DECLARE s INT := 0;
BEGIN
  <<outer>> FOR i IN 1..5 LOOP
    FOR j IN 1..5 LOOP
      CONTINUE outer WHEN j > i;
      s := s + 1;
      EXIT outer WHEN s > 9;
    END LOOP;
  END LOOP;
  <<blk>> BEGIN s := s * 10; EXIT blk; s := 0; END;
  RETURN s;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lbl();",
                    expected: Expected::Rows {
                        columns: &[Column("lbl", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fib(n INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN IF n < 2 THEN RETURN n; END IF; RETURN fib(n - 1) + fib(n - 2); END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fib(15);",
                    expected: Expected::Rows {
                        columns: &[Column("fib", INT4)],
                        rows: &[
                            &[T("610")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION noret(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN IF x > 0 THEN RETURN 1; END IF; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT noret(1);",
                    expected: Expected::Rows {
                        columns: &[Column("noret", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT noret(0);",
                    expected: Expected::Error(Diagnostic { code: "2F005", message: "control reached end of function without RETURN", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION nocase(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN CASE x WHEN 1 THEN RETURN 1; END CASE; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nocase(2);",
                    expected: Expected::Error(Diagnostic { code: "20000", message: "case not found", hint: "CASE statement is missing ELSE part.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION divz(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 10 / x; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT divz(0);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION loopy(n INT) RETURNS BIGINT LANGUAGE plpgsql AS $$DECLARE s BIGINT := 0; BEGIN FOR i IN 1..n LOOP s := s + i * i; END LOOP; RETURN s; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loopy(10000);",
                    expected: Expected::Rows {
                        columns: &[Column("loopy", INT8)],
                        rows: &[
                            &[T("333383335000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION nullbound() RETURNS INT LANGUAGE plpgsql AS $$BEGIN FOR i IN 1..NULL LOOP END LOOP; RETURN 0; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nullbound();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "upper bound of FOR loop cannot be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION badstep() RETURNS INT LANGUAGE plpgsql AS $$BEGIN FOR i IN 1..3 BY 0 LOOP END LOOP; RETURN 0; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT badstep();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "BY value of FOR loop must be greater than zero", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL execution: variables, casts, NOT NULL, polymorphism, PERFORM, and FOUND",
            set_up_script: &[
                "CREATE TABLE t (a INT);",
                "INSERT INTO t VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION casts() RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE i INT; n NUMERIC(5,2); t TEXT;
BEGIN i := 2.7; n := 3.14159; t := 12; RETURN i || ' ' || n || ' ' || t; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT casts();",
                    expected: Expected::Rows {
                        columns: &[Column("casts", TEXT)],
                        rows: &[
                            &[T("3 3.14 12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION toolong() RETURNS TEXT LANGUAGE plpgsql AS $$DECLARE v VARCHAR(3); BEGIN v := 'abcdef'; RETURN v; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT toolong();",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character varying(3)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION nn() RETURNS INT LANGUAGE plpgsql AS $$DECLARE v INT NOT NULL := 1; BEGIN v := NULL; RETURN v; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nn();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: r#"null value cannot be assigned to variable "v" declared NOT NULL"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION poly(a anyelement) RETURNS anyelement LANGUAGE plpgsql AS $$BEGIN RETURN a; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT poly(3), poly('x'::TEXT), poly(2.5);",
                    expected: Expected::Rows {
                        columns: &[Column("poly", INT4), Column("poly", TEXT), Column("poly", NUMERIC)],
                        rows: &[
                            &[T("3"), T("x"), T("2.5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fnd() RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE r TEXT := '';
BEGIN
  PERFORM 1 FROM t WHERE a = 1; r := r || found::TEXT;
  PERFORM 1 FROM t WHERE a = 9; r := r || ',' || found::TEXT;
  FOR i IN REVERSE 3..1 BY 2 LOOP r := r || ',' || i; END LOOP;
  r := r || ',' || found::TEXT;
  RETURN r;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fnd();",
                    expected: Expected::Rows {
                        columns: &[Column("fnd", TEXT)],
                        rows: &[
                            &[T("true,false,3,1,true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION cnt() RETURNS BIGINT LANGUAGE plpgsql AS $$BEGIN RETURN (SELECT count(*) FROM t); END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cnt();",
                    expected: Expected::Rows {
                        columns: &[Column("cnt", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION many() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN a FROM t; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT many();",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "query returned more than one row", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION twocol() RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN 1, 2; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT twocol();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query returned 2 columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION amb(a INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN (SELECT a FROM t LIMIT 1); END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amb(5);",
                    expected: Expected::Error(Diagnostic { code: "42702", message: r#"column reference "a" is ambiguous"#, detail: "It could refer to either a PL/pgSQL variable or a table column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION asrt(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN ASSERT x > 0, 'must be positive'; RETURN x; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT asrt(1);",
                    expected: Expected::Rows {
                        columns: &[Column("asrt", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT asrt(-1);",
                    expected: Expected::Error(Diagnostic { code: "P0004", message: "must be positive", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.check_asserts = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT asrt(-1);",
                    expected: Expected::Rows {
                        columns: &[Column("asrt", INT4)],
                        rows: &[
                            &[T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL execution: RAISE levels, formats, and options",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION noisy(x INT) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE 'x is %, half is %', x, x / 2.0;
  RAISE WARNING 'careful: %%';
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT noisy(5);",
                    expected: Expected::Rows {
                        columns: &[Column("noisy", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "x is 5, half is 2.5000000000000000", ..N }, Diagnostic { severity: "WARNING", code: "01000", message: "careful: %", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION boom(x INT) RETURNS INT LANGUAGE plpgsql AS $$
BEGIN
  IF x > 3 THEN RAISE EXCEPTION 'too big: %', x USING HINT = 'smaller', DETAIL = 'x is ' || x; END IF;
  RETURN x;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT boom(2);",
                    expected: Expected::Rows {
                        columns: &[Column("boom", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT boom(9);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "too big: 9", detail: "x is 9", hint: "smaller", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE division_by_zero; END$$;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division_by_zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE SQLSTATE 'U0001' USING MESSAGE = 'custom', COLUMN = 'c', TABLE = 't', SCHEMA = 's', CONSTRAINT = 'k', DATATYPE = 'd'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "U0001", message: "custom", schema: "s", table: "t", column: "c", data_type: "d", constraint: "k", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE NOTICE 'null param %', NULL; END$$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "null param <NULL>", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE 'plain %', 1 USING ERRCODE = 'unique_violation'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "plain 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE NOTICE 'x' USING HINT = NULL; END$$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "RAISE statement option cannot be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE NOTICE 'x' USING HINT = 'a', HINT = 'b'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "RAISE option already specified: HINT", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE; END$$;",
                    expected: Expected::Error(Diagnostic { code: "0Z002", message: "RAISE without parameters cannot be used outside an exception handler", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL execution: records, rows, OUT parameters, and composite results",
            set_up_script: &[
                "CREATE TYPE pair AS (a INT, b TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION mkpair(x INT) RETURNS pair LANGUAGE plpgsql AS $$
DECLARE p pair;
BEGIN p.a := x; p.b := 'v' || x; RETURN p; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mkpair(3);",
                    expected: Expected::Rows {
                        columns: &[Column("mkpair", USER_DEFINED)],
                        rows: &[
                            &[T("(3,v3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (mkpair(4)).b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("v4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION recpair(x INT) RETURNS pair LANGUAGE plpgsql AS $$
DECLARE r RECORD;
BEGIN r := ROW(x, 'r' || x)::pair; RETURN r; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT recpair(5);",
                    expected: Expected::Rows {
                        columns: &[Column("recpair", USER_DEFINED)],
                        rows: &[
                            &[T("(5,r5)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION outs(x INT, OUT y INT, OUT z TEXT) LANGUAGE plpgsql AS $$BEGIN y := x * 2; z := 'z' || x; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT outs(6);",
                    expected: Expected::Rows {
                        columns: &[Column("outs", RECORD)],
                        rows: &[
                            &[T("(12,z6)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION unassigned() RETURNS INT LANGUAGE plpgsql AS $$DECLARE r RECORD; BEGIN RETURN r.a; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unassigned();",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"record "r" is not assigned yet"#, detail: "The tuple structure of a not-yet-assigned record is indeterminate.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION nullrec() RETURNS TEXT LANGUAGE plpgsql AS $$DECLARE p pair; BEGIN RETURN coalesce(p.a::TEXT, 'null') || (p IS NULL)::TEXT; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nullrec();",
                    expected: Expected::Rows {
                        columns: &[Column("nullrec", TEXT)],
                        rows: &[
                            &[T("nulltrue")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION notcomp() RETURNS pair LANGUAGE plpgsql AS $$BEGIN RETURN 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT notcomp();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot return non-composite value from function returning composite type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION recarg(p pair) RETURNS TEXT LANGUAGE plpgsql AS $$BEGIN RETURN p.b || p.a; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT recarg(ROW(7, 'q')::pair);",
                    expected: Expected::Rows {
                        columns: &[Column("recarg", TEXT)],
                        rows: &[
                            &[T("q7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL execution: DO blocks and FOREACH",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT := 2; BEGIN x := x * 21; RAISE NOTICE 'x=%', x; END$$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "x=42", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE EXCEPTION 'do failed %', 1 + 1; END$$;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "do failed 2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; s INT := 0; BEGIN FOREACH x IN ARRAY ARRAY[1,2,3,NULL,5] LOOP s := s + coalesce(x, 100); END LOOP; RAISE NOTICE 's=% found=%', s, found; END$$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "s=111 found=t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE a INT[]; BEGIN FOREACH a SLICE 1 IN ARRAY ARRAY[[1,2],[3,4]] LOOP RAISE NOTICE 'slice %', a; END LOOP; END$$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "slice {1,2}", ..N }, Diagnostic { code: "00000", message: "slice {3,4}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; BEGIN FOREACH x IN ARRAY NULL::INT[] LOOP END LOOP; END$$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "FOREACH expression must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; BEGIN FOREACH x IN ARRAY 5 LOOP END LOOP; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "FOREACH expression must yield an array, not type integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; BEGIN FOREACH x SLICE 3 IN ARRAY ARRAY[1] LOOP END LOOP; END$$;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "slice dimension (3) is out of the valid range 0..1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT[]; BEGIN FOREACH x IN ARRAY ARRAY[1] LOOP END LOOP; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "FOREACH loop variable must not be of an array type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$DECLARE x INT; BEGIN FOREACH x SLICE 1 IN ARRAY ARRAY[1] LOOP END LOOP; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "FOREACH ... SLICE loop variable must be of an array type", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL execution: CREATE OR REPLACE replaces a compiled function",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN x + 1; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r(1);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION r(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN x + 100; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r(1);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION r(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN x * 1000; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r(1);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("1000")],
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
                    query: "SELECT r(1);",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION r(INT) RENAME TO r3;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r3(2);",
                    expected: Expected::Rows {
                        columns: &[Column("r3", INT4)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OR REPLACE FUNCTION keeps what callers depend on",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r(x INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN x; END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION r(x INT) RETURNS TEXT LANGUAGE plpgsql AS $$BEGIN RETURN 'x'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot change return type of existing function", hint: "Use DROP FUNCTION r(integer) first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION r(y INT) RETURNS INT LANGUAGE plpgsql AS $$BEGIN RETURN y; END$$;",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: r#"cannot change name of input parameter "x""#, hint: "Use DROP FUNCTION r(integer) first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE PROCEDURE r(x INT) LANGUAGE plpgsql AS $$BEGIN END$$;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot change routine kind", detail: r#""r" is a function."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION d(a INT, b INT DEFAULT 1) RETURNS INT LANGUAGE sql AS 'SELECT a + b';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION d(a INT, b INT) RETURNS INT LANGUAGE sql AS 'SELECT a + b';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot remove parameter defaults from existing function", hint: "Use DROP FUNCTION d(integer,integer) first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION d(a INT, b INT DEFAULT 2::BIGINT) RETURNS INT LANGUAGE sql AS 'SELECT a + b';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION d(a INT, b INT DEFAULT 5) RETURNS INT LANGUAGE sql AS 'SELECT a * b';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d(3);",
                    expected: Expected::Rows {
                        columns: &[Column("d", INT4)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION o(OUT a INT, OUT b INT) LANGUAGE sql AS 'SELECT 1, 2';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION o(OUT a INT, OUT c INT) LANGUAGE sql AS 'SELECT 1, 2';",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "cannot change return type of existing function", detail: "Row type defined by OUT parameters is different.", hint: "Use DROP FUNCTION o() first.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_execution_pending() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL execution: a call keeps running the version it started when CREATE OR REPLACE replaces it",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION selfrep() RETURNS INT LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE 'CREATE OR REPLACE FUNCTION selfrep() RETURNS INT LANGUAGE plpgsql AS ''BEGIN RETURN 2; END''';
  RETURN 1;
END$$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT selfrep();",
                    expected: Expected::Rows {
                        columns: &[Column("selfrep", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT selfrep();",
                    expected: Expected::Rows {
                        columns: &[Column("selfrep", INT4)],
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
        ScriptTest {
            name: "PL/pgSQL execution: a function compiled on one branch never runs on another where its body differs",
            set_up_script: &[
                "CREATE FUNCTION bf() RETURNS TEXT LANGUAGE plpgsql AS $$BEGIN RETURN 'main'; END$$;",
                "SELECT dolt_commit('-Am', 'add bf');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_checkout('other');",
                "CREATE OR REPLACE FUNCTION bf() RETURNS TEXT LANGUAGE plpgsql AS $$BEGIN RETURN 'other'; END$$;",
                "SELECT dolt_commit('-Am', 'change bf');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT bf();",
                    expected: Expected::Rows {
                        columns: &[Column("bf", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
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
                    query: "SELECT bf();",
                    expected: Expected::Rows {
                        columns: &[Column("bf", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
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
                    query: "SELECT bf();",
                    expected: Expected::Rows {
                        columns: &[Column("bf", TEXT)],
                        rows: &[
                            &[T("other")],
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
fn test_plpgsql_sql_statements() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL SQL statements: INSERT, UPDATE, DELETE, and SELECT set FOUND and fill INTO's target",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO t VALUES (1, 'x'), (2, 'y'); RAISE NOTICE 'found %', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "found t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN UPDATE t SET b = 'z' WHERE a = 5; RAISE NOTICE 'found %', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "found f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DELETE FROM t WHERE a = 5; RAISE NOTICE 'found %', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "found f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO v FROM t WHERE a = 1; RAISE NOTICE 'v % %', v, FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v x t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN v := 'old'; SELECT b INTO v FROM t WHERE a = 9; RAISE NOTICE 'v % %', v, FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v <NULL> f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO v FROM t ORDER BY a DESC; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v y", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN SELECT * INTO r FROM t WHERE a = 2; RAISE NOTICE 'r % %', r.a, r.b; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "r 2 y", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE x INT; y TEXT; BEGIN SELECT a, b INTO x, y FROM t WHERE a = 2; RAISE NOTICE '% %', x, y; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2 y", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r t; BEGIN SELECT * INTO r FROM t WHERE a = 1; RAISE NOTICE '%', r; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "(1,x)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN INSERT INTO t VALUES (3, 'w') RETURNING a INTO v; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v 3", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN UPDATE t SET b = b RETURNING a INTO v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN DELETE FROM t WHERE a = -1 RETURNING a INTO v; RAISE NOTICE 'v % %', v, FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v <NULL> f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SELECT 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query has no destination for result data", hint: "If you want to discard the results of a SELECT, use PERFORM instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO t VALUES (4, 'q') RETURNING a; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query has no destination for result data", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN CREATE TABLE x8 (a INT) INTO v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INTO used with a command that cannot return data", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION fact(n INT) RETURNS INT LANGUAGE plpgsql AS $$ DECLARE r INT; BEGIN IF n <= 1 THEN RETURN 1; END IF; SELECT n * fact(n - 1) INTO r; RETURN r; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fact(5);",
                    expected: Expected::Rows {
                        columns: &[Column("fact", INT4)],
                        rows: &[
                            &[T("120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("w")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: STRICT, print_strict_params, and the too_many_rows check",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'x'), (2, 'y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO STRICT v FROM t WHERE a = 9; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO STRICT v FROM t; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO STRICT v FROM t WHERE a = 2; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v y", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN UPDATE t SET b = b RETURNING a INTO STRICT v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN DELETE FROM t WHERE a = -1 RETURNING a INTO STRICT v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp(k INT, n TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$
#print_strict_params on
DECLARE v TEXT; BEGIN SELECT b INTO STRICT v FROM t WHERE a > k AND n IS NOT NULL; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp(10, 'it''s');",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", detail: "parameters: k = '10', n = 'it''s'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp(0, 'x');",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", detail: "parameters: k = '0', n = 'x'", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp(1, 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("sp", TEXT)],
                        rows: &[
                            &[T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.print_strict_params = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO STRICT v FROM t WHERE a > 5; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; k INT := 7; BEGIN SELECT b INTO STRICT v FROM t WHERE a > k; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", detail: "parameters: k = '7'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plpgsql.print_strict_params;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings = 'too_many_rows';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO v FROM t ORDER BY a; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { severity: "WARNING", code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..N }, Diagnostic { code: "00000", message: "v x", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_errors = 'too_many_rows';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SELECT b INTO v FROM t; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", hint: "Make sure the query returns a single row, or use LIMIT 1.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: utility statements run inside functions",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "CREATE SCHEMA s;",
                "CREATE TABLE s.t (a INT);",
                "INSERT INTO s.t VALUES (1), (2);",
                "INSERT INTO t VALUES (10, 'a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE TABLE u (a INT); INSERT INTO u VALUES (1); RAISE NOTICE 'n %', (SELECT count(*) FROM u); END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "n 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ddl() RETURNS INT LANGUAGE plpgsql AS $$ DECLARE v INT; BEGIN CREATE TABLE d1 (a INT); INSERT INTO d1 VALUES (1); ALTER TABLE d1 ADD COLUMN b INT; UPDATE d1 SET b = 7; SELECT b INTO v FROM d1; DROP TABLE d1; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ddl();",
                    expected: Expected::Rows {
                        columns: &[Column("ddl", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ddl();",
                    expected: Expected::Rows {
                        columns: &[Column("ddl", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN SHOW work_mem INTO v; RAISE NOTICE 'work_mem %', v; SET work_mem = '8MB'; SHOW work_mem INTO v; RAISE NOTICE 'work_mem %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "work_mem 4MB", ..N }, Diagnostic { code: "00000", message: "work_mem 8MB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("8MB")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN SHOW ALL INTO r; RAISE NOTICE '%', r.name; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "allow_alter_system", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXPLAIN (COSTS OFF) SELECT * FROM t WHERE a = 10 INTO v; RAISE NOTICE 'plan %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "plan Index Scan using t_pkey on t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f() RETURNS BIGINT LANGUAGE plpgsql AS $$ DECLARE n BIGINT; BEGIN SET LOCAL search_path = s; SELECT count(*) INTO n FROM t; RETURN n; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f();",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T("s")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T(r#""$user", public"#)],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g() RETURNS VOID LANGUAGE plpgsql AS $$ BEGIN LOCK TABLE t IN SHARE MODE; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g();",
                    expected: Expected::Rows {
                        columns: &[Column("g", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p (a INT PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION h() RETURNS VOID LANGUAGE plpgsql AS $$ BEGIN SET CONSTRAINTS ALL DEFERRED; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT h();",
                    expected: Expected::Rows {
                        columns: &[Column("h", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET LOCAL work_mem = '1MB'; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "SET TRANSACTION ISOLATION LEVEL must be called before any query", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "SET TRANSACTION ISOLATION LEVEL must be called before any query", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DISCARD PLANS; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DISCARD SEQUENCES; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION mkf() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN CREATE FUNCTION inner_f() RETURNS INT LANGUAGE sql AS 'SELECT 41'; RETURN inner_f() + 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mkf();",
                    expected: Expected::Rows {
                        columns: &[Column("mkf", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE INDEX CONCURRENTLY ti ON t (a); END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE INDEX CONCURRENTLY cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DO 'BEGIN RAISE NOTICE ''nested''; END'; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "nested", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: ANALYZE inside a function",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'v'), (2, 'v'), (3, 'v'), (4, 'v'), (5, 'v'), (6, 'v'), (7, 'v'), (8, 'v'), (9, 'v'), (10, 'v'), (11, 'v'), (12, 'v'), (13, 'v'), (14, 'v'), (15, 'v'), (16, 'v'), (17, 'v'), (18, 'v'), (19, 'v'), (20, 'v'), (21, 'v'), (22, 'v'), (23, 'v'), (24, 'v'), (25, 'v'), (26, 'v'), (27, 'v'), (28, 'v'), (29, 'v'), (30, 'v'), (31, 'v'), (32, 'v'), (33, 'v'), (34, 'v'), (35, 'v'), (36, 'v'), (37, 'v'), (38, 'v'), (39, 'v'), (40, 'v'), (41, 'v'), (42, 'v'), (43, 'v'), (44, 'v'), (45, 'v'), (46, 'v'), (47, 'v'), (48, 'v'), (49, 'v'), (50, 'v');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION an() RETURNS REAL LANGUAGE plpgsql AS $$ DECLARE n REAL; BEGIN ANALYZE t; SELECT reltuples INTO n FROM pg_class WHERE relname = 't'; RETURN n; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT an();",
                    expected: Expected::Rows {
                        columns: &[Column("an", FLOAT4)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN ANALYZE; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN ANALYZE (VERBOSE OFF) t (a); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: statements a function cannot run",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN VACUUM t; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "VACUUM cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN VACUUM (ANALYZE) t; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "VACUUM cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SAVEPOINT s; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported transaction command in PL/pgSQL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN RELEASE SAVEPOINT s; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported transaction command in PL/pgSQL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN COPY t TO STDOUT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot COPY to/from client in PL/pgSQL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN COPY t FROM STDIN; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot COPY to/from client in PL/pgSQL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE DATABASE nope; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE DATABASE cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DROP DATABASE nope; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DROP DATABASE cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN DISCARD ALL; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DISCARD ALL cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'SAVEPOINT s'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "EXECUTE of transaction commands is not implemented", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'BEGIN'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "EXECUTE of transaction commands is not implemented", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'COPY t TO STDOUT'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot COPY to/from client in PL/pgSQL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'VACUUM t'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "VACUUM cannot be executed from a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION st() RETURNS VOID LANGUAGE plpgsql STABLE AS $$ BEGIN CREATE TABLE nope2 (a INT); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT st();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CREATE TABLE is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION st2() RETURNS TEXT LANGUAGE plpgsql STABLE AS $$ DECLARE v TEXT; BEGIN SHOW work_mem INTO v; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT st2();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "SHOW is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION st3() RETURNS VOID LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN EXECUTE 'SET work_mem = ''2MB'''; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT st3();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "SET is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE TABLE ro (a INT); END $$;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute CREATE TABLE in a read-only transaction", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: dynamic EXECUTE",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'CREATE TABLE w (a INT); INSERT INTO w VALUES ($1)' USING 5; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'INSERT INTO w VALUES ($1), ($2)' USING 6, 7; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM w ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'SELECT max(a) FROM w WHERE a < $1' INTO v USING 100; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v 7", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'SELECT a FROM w ORDER BY a' INTO v; RAISE NOTICE 'v % %', v, FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v 5 f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'SELECT a FROM w' INTO STRICT v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0003", message: "query returned more than one row", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'SELECT a FROM w WHERE a > $1' INTO STRICT v USING 100; END $$;",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dsp(k INT) RETURNS INT LANGUAGE plpgsql AS $$
#print_strict_params on
DECLARE v INT; BEGIN EXECUTE 'SELECT a FROM w WHERE a > $1 AND $2 IS NOT NULL' INTO STRICT v USING k, NULL::TEXT; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dsp(100);",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", detail: "parameters: $1 = '100', $2 = NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dsp(0);",
                    expected: Expected::Error(Diagnostic { code: "P0002", message: "query returned no rows", detail: "parameters: $1 = '0', $2 = NULL", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE NULL; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "query string argument of EXECUTE is null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE ''; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXECUTE 'SELECT $1' INTO v USING 'abc'; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "abc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXECUTE 'SELECT $1 || 1' INTO v USING 'abc'; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "abc1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXECUTE 'SHOW work_mem' INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "4MB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'CREATE TABLE x9 (a INT)' INTO v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INTO used with a command that cannot return data", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'INSERT INTO w VALUES (8)' INTO v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INTO used with a command that cannot return data", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'INSERT INTO w VALUES (9) RETURNING a' INTO v; RAISE NOTICE 'v %', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "v 9", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'SELEC 1'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "SELEC""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION mk(name TEXT) RETURNS VOID LANGUAGE plpgsql AS $$ BEGIN EXECUTE format('CREATE TABLE %I (x INT)', name); EXECUTE format('INSERT INTO %I VALUES (1), (2)', name); END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mk('m1');",
                    expected: Expected::Rows {
                        columns: &[Column("mk", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM m1 ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION cnt(name TEXT) RETURNS BIGINT LANGUAGE plpgsql AS $$ DECLARE n BIGINT; BEGIN EXECUTE 'SELECT count(*) FROM ' || quote_ident(name) INTO n; RETURN n; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cnt('m1');",
                    expected: Expected::Rows {
                        columns: &[Column("cnt", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cnt('nope');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'SELECT 1 INTO TEMP zz'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "EXECUTE of SELECT ... INTO is not implemented", hint: "You might want to use EXECUTE ... INTO or EXECUTE CREATE TABLE ... AS instead.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL SQL statements: EXPLAIN EXECUTE of the session's prepared statement",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE pq(INT) AS SELECT b FROM t WHERE a = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXPLAIN (COSTS OFF) EXECUTE pq(1) INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "Index Scan using t_pkey on t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ee(k INT) RETURNS SETOF TEXT LANGUAGE sql AS $$ SELECT 'x' $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE pq(1)' INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "Index Scan using t_pkey on t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXPLAIN (COSTS OFF) EXECUTE nope(1) INTO v; END $$;",
                    expected: Expected::Error(Diagnostic { code: "26000", message: r#"prepared statement "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE pq(1);",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("x")],
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
fn test_plpgsql_cursors() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL cursors: FOR over a query, a bound cursor, and EXECUTE",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN SELECT a, b FROM t ORDER BY a LOOP RAISE NOTICE '% %', r.a, r.b; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "1 one", ..N }, Diagnostic { code: "00000", message: "2 two", ..N }, Diagnostic { code: "00000", message: "3 three", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE x INT; y TEXT; BEGIN FOR x, y IN SELECT a, b FROM t WHERE a > 1 ORDER BY a LOOP RAISE NOTICE '%=%', x, y; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2=two", ..N }, Diagnostic { code: "00000", message: "3=three", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN SELECT * FROM t WHERE a > 10 LOOP END LOOP; RAISE NOTICE 'found %', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "found f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION upto(k INT) RETURNS INT LANGUAGE plpgsql AS $$ DECLARE r RECORD; s INT := 0; BEGIN FOR r IN SELECT a FROM t ORDER BY a LOOP EXIT WHEN r.a > k; s := s + r.a; END LOOP; RETURN s; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT upto(2);",
                    expected: Expected::Rows {
                        columns: &[Column("upto", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN SELECT a FROM t ORDER BY a LOOP RAISE NOTICE '%', r.a; CONTINUE WHEN r.a = 1; EXIT; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "1", ..N }, Diagnostic { code: "00000", message: "2", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN EXECUTE 'SELECT a FROM t WHERE a >= $1 ORDER BY a' USING 2 LOOP RAISE NOTICE '%', r.a; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2", ..N }, Diagnostic { code: "00000", message: "3", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR (k INT) FOR SELECT b FROM t WHERE a > k ORDER BY a; BEGIN FOR r IN c(1) LOOP RAISE NOTICE '%', r.b; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "two", ..N }, Diagnostic { code: "00000", message: "three", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR (k INT) FOR SELECT b FROM t WHERE a > k ORDER BY a; BEGIN FOR r IN c(k := 2) LOOP RAISE NOTICE '%', r.b; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "three", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN UPDATE t SET b = upper(b) WHERE a = 2 RETURNING a, b LOOP RAISE NOTICE '% %', r.a, r.b; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2 TWO", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE l TEXT; BEGIN FOR l IN EXPLAIN (COSTS OFF) SELECT * FROM t WHERE a = 1 LOOP RAISE NOTICE '%', l; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "Index Scan using t_pkey on t", ..N }, Diagnostic { code: "00000", message: "  Index Cond: (a = 1)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN INSERT INTO t VALUES (9, 'x') LOOP END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot open INSERT query as cursor", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN EXECUTE 'INSERT INTO t VALUES (9, ''x'')' LOOP END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot open INSERT query as cursor", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR FOR SELECT a FROM t; BEGIN c := 'cc'; FOR r IN c LOOP CLOSE c; END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cannot drop pinned portal "cc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN EXECUTE NULL LOOP END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "query string argument of EXECUTE is null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("TWO")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL cursors: OPEN, FETCH, MOVE, and CLOSE",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; BEGIN OPEN c FOR SELECT a FROM t ORDER BY a; FETCH c INTO x; RAISE NOTICE '% %', x, FOUND; FETCH LAST FROM c INTO x; RAISE NOTICE '%', x; FETCH PRIOR FROM c INTO x; RAISE NOTICE '%', x; MOVE ABSOLUTE 0 FROM c; FETCH NEXT FROM c INTO x; RAISE NOTICE '%', x; FETCH RELATIVE 5 FROM c INTO x; RAISE NOTICE '% %', x, FOUND; CLOSE c; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "1 t", ..N }, Diagnostic { code: "00000", message: "3", ..N }, Diagnostic { code: "00000", message: "2", ..N }, Diagnostic { code: "00000", message: "1", ..N }, Diagnostic { code: "00000", message: "<NULL> f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; n INT := 2; BEGIN OPEN c SCROLL FOR SELECT a FROM t ORDER BY a; FETCH ABSOLUTE n FROM c INTO x; RAISE NOTICE '%', x; MOVE FORWARD ALL IN c; RAISE NOTICE '%', FOUND; MOVE BACKWARD 2 FROM c; FETCH c INTO x; RAISE NOTICE '%', x; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2", ..N }, Diagnostic { code: "00000", message: "t", ..N }, Diagnostic { code: "00000", message: "3", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; BEGIN OPEN c NO SCROLL FOR SELECT a FROM t ORDER BY a; FETCH LAST FROM c INTO x; FETCH PRIOR FROM c INTO x; END $$;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; n INT; BEGIN OPEN c FOR SELECT a FROM t ORDER BY a; FETCH ABSOLUTE n FROM c INTO x; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "relative or absolute cursor position is null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; BEGIN OPEN c FOR SELECT a FROM t ORDER BY a; FETCH c INTO x; FETCH c INTO x; MOVE c; RAISE NOTICE '%', FOUND; MOVE c; RAISE NOTICE '%', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "t", ..N }, Diagnostic { code: "00000", message: "f", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION opener() RETURNS REFCURSOR LANGUAGE plpgsql AS $$ DECLARE c REFCURSOR := 'mycur'; BEGIN OPEN c FOR SELECT b FROM t ORDER BY a; RETURN c; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT opener();",
                    expected: Expected::Rows {
                        columns: &[Column("opener", REFCURSOR)],
                        rows: &[
                            &[T("mycur")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM mycur;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("one")],
                            &[T("two")],
                            &[T("three")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT opener();",
                    expected: Expected::Error(Diagnostic { code: "42P03", message: r#"cursor "mycur" already in use"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; BEGIN CLOSE c; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: r#"cursor variable "c" is null"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; x INT; BEGIN FETCH c INTO x; END $$;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: r#"cursor variable "c" is null"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR := 'nope'; BEGIN CLOSE c; END $$;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR := 'nope'; x INT; BEGIN MOVE c; END $$;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR (k INT) FOR SELECT b FROM t WHERE a = k; v TEXT; BEGIN OPEN c(3); FETCH c INTO v; RAISE NOTICE '%', v; CLOSE c; OPEN c(k := 1); FETCH c INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "three", ..N }, Diagnostic { code: "00000", message: "one", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; r RECORD; BEGIN OPEN c FOR EXECUTE 'SELECT a, b FROM t WHERE a = $1' USING 2; FETCH c INTO r; RAISE NOTICE '%', r; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "(2,two)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR FOR SELECT a, b FROM t ORDER BY a FOR UPDATE; r RECORD; BEGIN OPEN c; FETCH c INTO r; UPDATE t SET b = 'first' WHERE CURRENT OF c; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("first")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; BEGIN OPEN c SCROLL FOR SELECT a FROM t FOR UPDATE; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DECLARE SCROLL CURSOR ... FOR UPDATE/SHARE is not supported", detail: "Scrollable cursors must be READ ONLY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; r RECORD; BEGIN OPEN c FOR UPDATE t SET b = b || '!' WHERE a = 3 RETURNING *; FETCH c INTO r; RAISE NOTICE '%', r; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "(3,three!)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION stablecur() RETURNS INT STABLE LANGUAGE plpgsql AS $$ DECLARE c REFCURSOR; BEGIN OPEN c FOR UPDATE t SET b = b WHERE a = 3 RETURNING *; RETURN 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stablecur();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "UPDATE is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; l TEXT; BEGIN OPEN c FOR EXPLAIN (COSTS OFF) SELECT * FROM t WHERE a = 1; FETCH c INTO l; RAISE NOTICE '%', l; MOVE c; RAISE NOTICE '%', FOUND; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "Index Scan using t_pkey on t", ..N }, Diagnostic { code: "00000", message: "t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; r RECORD; BEGIN OPEN c FOR EXECUTE 'SHOW datestyle'; FETCH c INTO r; RAISE NOTICE '%', r; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: r#"("ISO, MDY")"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; BEGIN OPEN c FOR EXECUTE 'SELECT 1; SELECT 2'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot open multi-query plan as cursor", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL cursors: PREPARE, EXECUTE, and DEALLOCATE inside functions",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'PREPARE p1(INT) AS SELECT b FROM t WHERE a = $1'; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p1(1);",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v TEXT; BEGIN EXECUTE 'EXECUTE p1(2)' INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "two", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN EXECUTE 'EXECUTE p1(3)' LOOP RAISE NOTICE '%', r.b; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "three", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'PREPARE p2(INT) AS INSERT INTO t VALUES ($1, ''new'')'; EXECUTE 'EXECUTE p2(4)'; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                            &[T("4"), T("new")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'PREPARE p1 AS SELECT 1'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42P05", message: r#"prepared statement "p1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'DEALLOCATE p1'; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p1(1);",
                    expected: Expected::Error(Diagnostic { code: "26000", message: r#"prepared statement "p1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'DEALLOCATE p1'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "26000", message: r#"prepared statement "p1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE 'DEALLOCATE ALL'; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p2(5);",
                    expected: Expected::Error(Diagnostic { code: "26000", message: r#"prepared statement "p2" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sq() RETURNS INT LANGUAGE plpgsql STABLE AS $$ DECLARE v INT; BEGIN EXECUTE 'EXECUTE p3' INTO v; RETURN v; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p3 AS SELECT 7;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sq();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "EXECUTE is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p4 AS INSERT INTO t VALUES (8, 'eight') RETURNING a;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE v INT; BEGIN EXECUTE 'EXECUTE p4' INTO v; RAISE NOTICE '%', v; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "8", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sq2() RETURNS INT LANGUAGE plpgsql STABLE AS $$ BEGIN EXECUTE 'DEALLOCATE p3'; RETURN 1; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sq2();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DEALLOCATE is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c REFCURSOR; r RECORD; BEGIN OPEN c FOR EXECUTE 'EXECUTE p3'; FETCH c INTO r; RAISE NOTICE '%', r; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "(7)", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL cursors: CLOSE ALL refuses a pinned portal",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ DECLARE c CURSOR FOR SELECT 1; BEGIN c := 'cc'; FOR r IN c LOOP EXECUTE 'CLOSE ALL'; END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cannot drop pinned portal "cc""#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_exception_handlers() {
    run_scripts(&[
        ScriptTest {
            name: "exception handlers run in subtransactions that roll back what their block did",
            set_up_script: &[
                "CREATE TABLE et (a INT PRIMARY KEY, b INT CHECK (b > 0), c TEXT NOT NULL DEFAULT 'x');",
                "CREATE DOMAIN posint AS INT CHECK (VALUE > 0);",
                "CREATE SEQUENCE es;",
                "CREATE FUNCTION ef(x INT) RETURNS BIGINT LANGUAGE plpgsql AS $$ BEGIN INSERT INTO et VALUES (x, 1); BEGIN INSERT INTO et VALUES (x + 1, 1); PERFORM 1 / (x - x); EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'caught % %', SQLSTATE, SQLERRM; END; RETURN (SELECT count(*) FROM et); END $$;",
                "CREATE FUNCTION diag(sql TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE s TEXT; m TEXT; d TEXT; h TEXT; col TEXT; con TEXT; dt TEXT; tab TEXT; sch TEXT; BEGIN EXECUTE sql; RETURN 'none'; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS s = RETURNED_SQLSTATE, m = MESSAGE_TEXT, d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT, col = COLUMN_NAME, con = CONSTRAINT_NAME, dt = PG_DATATYPE_NAME, tab = TABLE_NAME, sch = SCHEMA_NAME; RETURN concat_ws(' | ', s, m, d, h, col, con, dt, tab, sch); END $$;",
                "CREATE FUNCTION ctx() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE c TEXT; BEGIN INSERT INTO et VALUES (1, 1); RETURN 'none'; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT; RETURN c; END $$;",
                "CREATE FUNCTION reraise() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN BEGIN PERFORM 1 / 0; EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'inner %', SQLSTATE; RAISE; END; RETURN 1; END $$;",
                "CREATE FUNCTION rc() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE n BIGINT; o OID; c TEXT; BEGIN UPDATE et SET b = b; GET DIAGNOSTICS n = ROW_COUNT, o = PG_ROUTINE_OID, c = PG_CONTEXT; RETURN n || ' ' || (o = 'rc'::regproc::oid) || ' ' || c; END $$;",
                "CREATE FUNCTION ddl() RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE r TEXT; BEGIN BEGIN CREATE TABLE ddl_t (a INT); RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN NULL; END; CREATE TABLE ddl_t (b TEXT); INSERT INTO ddl_t VALUES ('ok'); SELECT b INTO r FROM ddl_t; RETURN r; END $$;",
                "CREATE TABLE dp (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED);",
                "INSERT INTO dp VALUES (1);",
                "CREATE TABLE fp (a INT PRIMARY KEY);",
                "CREATE TABLE fc (a INT REFERENCES fp DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ef(1);",
                    expected: Expected::Rows {
                        columns: &[Column("ef", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "caught 22012 division by zero", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO et VALUES (1, 1); EXCEPTION WHEN integrity_constraint_violation THEN RAISE NOTICE 'class % %', SQLSTATE, SQLERRM; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: r#"class 23505 duplicate key value violates unique constraint "et_pkey""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN BEGIN PERFORM 1 / 0; EXCEPTION WHEN unique_violation THEN RAISE NOTICE 'inner'; END; EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'outer %', SQLSTATE; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "outer 22012", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT diag('INSERT INTO et VALUES (10, -1)');",
                    expected: Expected::Rows {
                        columns: &[Column("diag", TEXT)],
                        rows: &[
                            &[T(r#"23514 | new row for relation "et" violates check constraint "et_b_check" | Failing row contains (10, -1, x). |  |  | et_b_check |  | et | public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT diag('INSERT INTO et VALUES (11, 1, NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("diag", TEXT)],
                        rows: &[
                            &[T(r#"23502 | null value in column "c" of relation "et" violates not-null constraint | Failing row contains (11, 1, null). |  | c |  |  | et | public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT diag('INSERT INTO et VALUES (1, 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("diag", TEXT)],
                        rows: &[
                            &[T(r#"23505 | duplicate key value violates unique constraint "et_pkey" | Key (a)=(1) already exists. |  |  | et_pkey |  | et | public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT diag('SELECT (-1)::posint');",
                    expected: Expected::Rows {
                        columns: &[Column("diag", TEXT)],
                        rows: &[
                            &[T(r#"23514 | value for domain posint violates check constraint "posint_check" |  |  |  | posint_check | posint |  | public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT diag('SELECT 1');",
                    expected: Expected::Rows {
                        columns: &[Column("diag", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctx();",
                    expected: Expected::Rows {
                        columns: &[Column("ctx", TEXT)],
                        rows: &[
                            &[T(r#"SQL statement "INSERT INTO et VALUES (1, 1)"
PL/pgSQL function ctx() line 1 at SQL statement"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reraise();",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    notices: &[Diagnostic { code: "00000", message: "inner 22012", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN PERFORM reraise(); EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'outer caught %', SQLERRM; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "inner 22012", ..N }, Diagnostic { code: "00000", message: "outer caught division by zero", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN RAISE; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0Z002", message: "RAISE without parameters cannot be used outside an exception handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s TEXT; BEGIN GET STACKED DIAGNOSTICS s = MESSAGE_TEXT; END $$;",
                    expected: Expected::Error(Diagnostic { code: "0Z002", message: "GET STACKED DIAGNOSTICS cannot be used outside an exception handler", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rc();",
                    expected: Expected::Rows {
                        columns: &[Column("rc", TEXT)],
                        rows: &[
                            &[T("1 true PL/pgSQL function rc() line 1 at GET DIAGNOSTICS")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN PERFORM nextval('es'); SET LOCAL work_mem = '77kB'; BEGIN PERFORM nextval('es'); SET LOCAL work_mem = '88kB'; RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN RAISE NOTICE '% %', currval('es'), current_setting('work_mem'); END; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "2 77kB", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ddl();",
                    expected: Expected::Rows {
                        columns: &[Column("ddl", TEXT)],
                        rows: &[
                            &[T("ok")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname FROM pg_attribute WHERE attrelid = 'ddl_t'::regclass AND attnum > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO dp VALUES (1); RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dp;",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO fc VALUES (5); RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM fc;",
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
                    query: "SET statement_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN PERFORM pg_sleep(1); EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'caught by others'; END $$;",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN PERFORM pg_sleep(1); EXCEPTION WHEN query_canceled THEN RAISE NOTICE 'caught cancel'; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "caught cancel", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET statement_timeout;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "locks taken in a rolled-back exception block are released while earlier ones stay held",
            set_up_script: &[
                "CREATE TABLE l1 (a INT);",
                "CREATE TABLE l2 (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE l1 IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN LOCK TABLE l2 IN ACCESS EXCLUSIVE MODE; RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE l2 IN ACCESS EXCLUSIVE MODE NOWAIT;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE l1 IN ACCESS EXCLUSIVE MODE NOWAIT;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: r#"could not obtain lock on relation "l1""#, ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_recursion_through_one_plan() {
    run_scripts(&[
        ScriptTest {
            name: "recursive statements run one shared plan, which an inner level invalidates",
            set_up_script: &[
                "CREATE TABLE spi_r (v int4);",
                "INSERT INTO spi_r SELECT generate_series(1, 10);",
                "CREATE TABLE spi_d (a int4);",
                "CREATE FUNCTION spi_rec(n int4) RETURNS int4 LANGUAGE plpgsql AS $$ DECLARE c int4; BEGIN IF n <= 0 THEN RETURN 0; END IF; SELECT count(*) + spi_rec(n - 1) INTO c FROM spi_r WHERE spi_r.v <= spi_rec.n; RETURN c; END $$;",
                "CREATE FUNCTION spi_rec_ddl(n int4) RETURNS text LANGUAGE plpgsql AS $$ DECLARE r text; BEGIN IF n > 0 THEN SELECT spi_rec_ddl(n - 1) INTO r FROM spi_r WHERE spi_r.v = 1; END IF; EXECUTE format('ALTER TABLE spi_d ADD COLUMN c%s int4', n); SELECT string_agg(attname, ',' ORDER BY attnum) INTO r FROM pg_attribute WHERE attrelid = 'spi_d'::regclass AND attnum > 0; RETURN r; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT spi_rec(1), spi_rec(5), spi_rec(100);",
                    expected: Expected::Rows {
                        columns: &[Column("spi_rec", INT4), Column("spi_rec", INT4), Column("spi_rec", INT4)],
                        rows: &[
                            &[T("1"), T("15"), T("955")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spi_rec_ddl(4);",
                    expected: Expected::Rows {
                        columns: &[Column("spi_rec_ddl", TEXT)],
                        rows: &[
                            &[T("a,c0,c1,c2,c3,c4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM spi_d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c0", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT spi_rec(3);",
                    expected: Expected::Rows {
                        columns: &[Column("spi_rec", INT4)],
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
    ]);
}

#[test]
fn test_plpgsql_validator_compile() {
    run_scripts(&[
        ScriptTest {
            name: "a function compiled by its validator keeps the signature formatted under the search_path of its CREATE, and one compiled for a call has the call's",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp6(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp6 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp7(a text) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp7 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp6(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp6 1", context: Some("PL/pgSQL function sp6(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp7('x');",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp7 x", context: Some("PL/pgSQL function public.sp7(text) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function whose CREATE rolled back leaves no compiled function for an identical one",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp8(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp8 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION public.sp8(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp8 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp8(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp8 1", context: Some("PL/pgSQL function public.sp8(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function created in a PL/pgSQL exception block that rolled back leaves no compiled function for an identical one",
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE FUNCTION sp9(a int4) RETURNS int4 LANGUAGE plpgsql AS $f$ BEGIN RAISE EXCEPTION 'sp9 %', a; END $f$; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION public.sp9(a int4) RETURNS int4 LANGUAGE plpgsql AS $f$ BEGIN RAISE EXCEPTION 'sp9 %', a; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp9(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp9 1", context: Some("PL/pgSQL function public.sp9(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function created in a released savepoint keeps the function its validator compiled",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp10(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp10 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT s;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp10(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp10 1", context: Some("PL/pgSQL function sp10(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function created in a savepoint rolled back to leaves no compiled function for an identical one",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp11(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp11 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION public.sp11(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp11 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp11(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp11 1", context: Some("PL/pgSQL function public.sp11(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OR REPLACE compiles the function again under its own search_path",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp12(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp12 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION public.sp12(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp12 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp12(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp12 1", context: Some("PL/pgSQL function public.sp12(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER FUNCTION leaves the function to compile again for its next call",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp13(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp13 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION public.sp13(int4) COST 5;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp13(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp13 1", context: Some("PL/pgSQL function public.sp13(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a rolled-back CREATE OR REPLACE of an identical function leaves no compiled function for the function it replaced",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a CREATE OR REPLACE of an identical function rolled back to a savepoint leaves no compiled function for the function it replaced",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a CREATE OR REPLACE of an identical function in a PL/pgSQL exception block that rolled back leaves no compiled function for the function it replaced",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN SET LOCAL search_path = public; CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $f$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $f$; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a committed CREATE OR REPLACE of an identical function keeps the function its validator compiled",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared CREATE OR REPLACE of an identical function that is rolled back leaves no compiled function for the function it replaced",
            server_config: r#"system_variables:
  max_prepared_transactions: 10
"#,
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'sp14';",
                    expected: Expected::Tag("PREPARE TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK PREPARED 'sp14';",
                    expected: Expected::Tag("ROLLBACK PREPARED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared CREATE OR REPLACE of an identical function that is committed keeps the function its validator compiled",
            server_config: r#"system_variables:
  max_prepared_transactions: 10
"#,
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'sp14';",
                    expected: Expected::Tag("PREPARE TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT PREPARED 'sp14';",
                    expected: Expected::Tag("COMMIT PREPARED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared CREATE OR REPLACE of an identical function called before it is committed compiles again for the call",
            server_config: r#"system_variables:
  max_prepared_transactions: 10
"#,
            assertions: &[
                ScriptTestAssertion {
                    query: "SET check_function_bodies = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET check_function_bodies;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL search_path = public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp14(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'sp14 %', a; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'sp14';",
                    expected: Expected::Tag("PREPARE TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT PREPARED 'sp14';",
                    expected: Expected::Tag("COMMIT PREPARED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.sp14(1);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "sp14 1", context: Some("PL/pgSQL function public.sp14(integer) line 1 at RAISE"), ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a function compiled on one branch compiles again after a checkout of a branch or a merge that changes its body",
            set_up_script: &[
                "CREATE FUNCTION sp15(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RETURN a + 1; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT sp15(1);",
                    expected: Expected::Rows {
                        columns: &[Column("sp15", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'sp15 adds one');",
                    expected: Expected::Rows { columns: &[Column("dolt_commit", TEXT)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION sp15(a int4) RETURNS int4 LANGUAGE plpgsql AS $$ BEGIN RETURN a + 2; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp15(1);",
                    expected: Expected::Rows {
                        columns: &[Column("sp15", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'sp15 adds two');",
                    expected: Expected::Rows { columns: &[Column("dolt_commit", TEXT)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp15(1);",
                    expected: Expected::Rows {
                        columns: &[Column("sp15", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other');",
                    expected: Expected::Rows { columns: &[Column("dolt_merge", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sp15(1);",
                    expected: Expected::Rows {
                        columns: &[Column("sp15", INT4)],
                        rows: &[
                            &[T("3")],
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
fn test_returned_values_an_exception_block_wrote() {
    run_scripts(&[
        ScriptTest {
            name: "values a rolled-back exception block wrote stay readable in the rows a function returned",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b text);",
                "CREATE TABLE t2 (a int, b text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION big() RETURNS text LANGUAGE sql AS $$ SELECT string_agg(md5(i::text), '') FROM generate_series(1, 3000) i $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g_query() RETURNS SETOF text LANGUAGE plpgsql AS $$ BEGIN BEGIN INSERT INTO t VALUES (13, big()); RETURN QUERY SELECT b FROM t WHERE a = 13; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(g_query()), (SELECT count(*) FROM t);",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("96000"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g_execute() RETURNS SETOF text LANGUAGE plpgsql AS $$ BEGIN BEGIN INSERT INTO t VALUES (14, big()); RETURN QUERY EXECUTE 'SELECT b FROM t WHERE a = $1' USING 14; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT md5(g_execute()) = md5(big());",
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
                    query: "CREATE FUNCTION g_next() RETURNS SETOF text LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN BEGIN INSERT INTO t VALUES (15, big()); FOR r IN SELECT b FROM t WHERE a = 15 LOOP RETURN NEXT r.b; END LOOP; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(g_next());",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g_cursor() RETURNS SETOF text LANGUAGE plpgsql AS $$ DECLARE c refcursor; v text; BEGIN BEGIN INSERT INTO t VALUES (16, big()); OPEN c FOR SELECT b FROM t WHERE a = 16; FETCH c INTO v; RETURN NEXT v; CLOSE c; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(g_cursor());",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g_nested() RETURNS SETOF text LANGUAGE plpgsql AS $$ BEGIN BEGIN BEGIN INSERT INTO t VALUES (17, big()); RETURN QUERY SELECT b FROM t WHERE a = 17; END; RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(g_nested());",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 SELECT 1, g_query();",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, length(b), md5(b) = md5(big()) FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("length", INT4), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("96000"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (18, big());",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION g_committed() RETURNS SETOF text LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT b FROM t WHERE a = 18; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(g_committed());",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "a cursor kept past its transaction keeps whole the values it read",
            set_up_script: &[
                "CREATE TABLE th (a int PRIMARY KEY, b text);",
                "CREATE FUNCTION big() RETURNS text LANGUAGE sql AS $$ SELECT string_agg(md5(i::text), '') FROM generate_series(1, 3000) i $$;",
                "CREATE FUNCTION fetch_length(name text) RETURNS int LANGUAGE plpgsql AS $$ DECLARE r refcursor := name; v text; BEGIN FETCH r INTO v; RETURN length(v); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO th VALUES (1, big());",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR WITH HOLD FOR SELECT b FROM th;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fetch_length('c');",
                    expected: Expected::Rows {
                        columns: &[Column("fetch_length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d CURSOR WITH HOLD FOR SELECT b FROM th;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM th;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fetch_length('d');",
                    expected: Expected::Rows {
                        columns: &[Column("fetch_length", INT4)],
                        rows: &[
                            &[T("96000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE d;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_settings_in_a_session_after_another_used_plpgsql() {
    run_scripts(&[
        ScriptTest {
            name: "a session's plpgsql settings take effect after another session loaded PL/pgSQL first",
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_warnings TO 'shadowed_variables';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION shadowtest(f1 INT) RETURNS INT AS $$ DECLARE f1 INT; BEGIN RETURN 1; END $$ LANGUAGE plpgsql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42712", message: r#"variable "f1" shadows a previously defined variable"#, position: 62, ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_load_plpgsql_reserves_its_prefix() {
    run_scripts(&[
        ScriptTest {
            name: "LOAD 'plpgsql' removes a placeholder plpgsql setting and reserves the prefix",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_foo_warnings = true;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD 'plpgsql';",
                    expected: Expected::Tag("LOAD"),
                    notices: &[Diagnostic {
                        severity: "WARNING",
                        code: "42602",
                        message: r#"invalid configuration parameter name "plpgsql.extra_foo_warnings", removing it"#,
                        detail: r#""plpgsql" is now a reserved prefix."#,
                        ..N
                    }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plpgsql.extra_foo_warnings = true;",
                    expected: Expected::Error(Diagnostic {
                        code: "42602",
                        message: r#"invalid configuration parameter name "plpgsql.extra_foo_warnings""#,
                        detail: r#""plpgsql" is a reserved prefix."#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD '$libdir/plugins/plpgsql';",
                    expected: Expected::Error(Diagnostic {
                        code: "58P01",
                        message: r#"could not access file "$libdir/plugins/plpgsql": No such file or directory"#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD 'nosuchlib';",
                    expected: Expected::Error(Diagnostic {
                        code: "58P01",
                        message: r#"could not access file "nosuchlib": No such file or directory"#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD '$libdir/no_such_library';",
                    expected: Expected::Error(Diagnostic {
                        code: "58P01",
                        message: r#"could not access file "$libdir/no_such_library": No such file or directory"#,
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "LOAD by a user who isn't a superuser names only a library under $libdir/plugins",
            set_up_script: &[
                "CREATE ROLE load_u",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE load_u;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD 'plpgsql';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"access to library "plpgsql" is not allowed"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD '$libdir/plugins/plpgsql';",
                    expected: Expected::Error(Diagnostic {
                        code: "58P01",
                        message: r#"could not access file "$libdir/plugins/plpgsql": No such file or directory"#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOAD '$libdir/plugins/x';",
                    expected: Expected::Error(Diagnostic {
                        code: "58P01",
                        message: r#"could not access file "$libdir/plugins/x": No such file or directory"#,
                        ..E
                    }),
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
fn test_plpgsql_catalogs_read_again_at_an_earlier_command() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL statements read the catalog of their own command after later commands wrote it",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t SELECT g, 'v' FROM generate_series(1, 50) g;",
                "CREATE FUNCTION an() RETURNS REAL LANGUAGE plpgsql AS $$ DECLARE n REAL; BEGIN ANALYZE t; SELECT reltuples INTO n FROM pg_class WHERE relname = 't'; RETURN n; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT an();",
                    expected: Expected::Rows {
                        columns: &[Column("an", FLOAT4)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN ANALYZE pg_type; ANALYZE pg_class; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN ANALYZE; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN FOR i IN 1..300 LOOP EXECUTE format('CREATE DOMAIN d%s AS INT DEFAULT 1', i); END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_type WHERE typname LIKE 'd%' AND typtype = 'd';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "debug:\n  check_every: true\n",
            ..S
        },
    ]);
}
