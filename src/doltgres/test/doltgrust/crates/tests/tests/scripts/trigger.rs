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
fn test_create_trigger() {
    run_scripts(&[
        ScriptTest {
            name: "BEFORE INSERT, with columns omitted from or reordered by the INSERT",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, selector TEXT DEFAULT 'DEFAULTED', result TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.result := 'saw_' || NEW.selector || '_' || NEW.pk::text;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, selector) VALUES (1, 'TRIGGER_B');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (selector, pk) VALUES ('DIRECT_A', 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("selector", TEXT), Column("result", TEXT)],
                        rows: &[
                            &[T("1"), T("TRIGGER_B"), T("saw_TRIGGER_B_1")],
                            &[T("2"), T("DIRECT_A"), T("saw_DIRECT_A_2")],
                            &[T("3"), T("DEFAULTED"), T("saw_DEFAULTED_3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE INSERT ... SELECT, with columns omitted from or reordered by the INSERT",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, selector TEXT DEFAULT 'DEFAULTED', result TEXT);",
                "CREATE TABLE source (pk INT PRIMARY KEY, selector TEXT);",
                "INSERT INTO source VALUES (1, 'FROM_QUERY'), (2, 'ALSO_FROM_QUERY');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.result := 'saw_' || NEW.selector || '_' || NEW.pk::text;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (selector, pk) SELECT selector, pk FROM source ORDER BY pk;",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) SELECT 3;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("selector", TEXT), Column("result", TEXT)],
                        rows: &[
                            &[T("1"), T("FROM_QUERY"), T("saw_FROM_QUERY_1")],
                            &[T("2"), T("ALSO_FROM_QUERY"), T("saw_ALSO_FROM_QUERY_2")],
                            &[T("3"), T("DEFAULTED"), T("saw_DEFAULTED_3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE INSERT",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi_1")],
                            &[T("2"), T("there_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE UPDATE",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
				RETURN NEW;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = v1 || '|' WHERE pk IN (1, 2);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi|_1")],
                            &[T("2"), T("there|_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE DELETE",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				INSERT INTO test2 VALUES (OLD.pk, OLD.v1);
				RETURN OLD;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE DELETE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE INSERT returning NULL",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
					INSERT INTO test2 VALUES (NEW.pk, NEW.v1);
					RETURN NULL;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi_1")],
                            &[T("2"), T("there_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE UPDATE returning NULL",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
				INSERT INTO test2 VALUES (NEW.pk, NEW.v1);
				RETURN NULL;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = v1 || '|' WHERE pk IN (1, 2);",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi|_1")],
                            &[T("2"), T("there|_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE DELETE returning NULL",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				INSERT INTO test2 VALUES (OLD.pk, OLD.v1);
				RETURN NULL;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE DELETE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE UPDATE with DELETE DML",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				INSERT INTO test2 VALUES (OLD.pk, OLD.v1);
				RETURN OLD;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AFTER INSERT",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
					INSERT INTO test2 VALUES (NEW.pk, NEW.v1);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger AFTER INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi_1")],
                            &[T("2"), T("there_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AFTER UPDATE",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
				INSERT INTO test2 VALUES (NEW.pk, NEW.v1);
				RETURN NEW;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger AFTER UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = v1 || '|' WHERE pk IN (1, 2);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi|")],
                            &[T("2"), T("there|")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi|_1")],
                            &[T("2"), T("there|_2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AFTER DELETE returning NULL",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			BEGIN
				INSERT INTO test2 VALUES (OLD.pk, OLD.v1);
				RETURN NULL;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger AFTER DELETE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Cascading DELETE into INSERT, different tables",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
BEGIN
	INSERT INTO test2 VALUES (OLD.pk, OLD.v1);
	RETURN OLD;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func2() RETURNS TRIGGER AS $$
BEGIN
	NEW.pk := NEW.pk + 100;
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE DELETE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test2 FOR EACH ROW EXECUTE FUNCTION trigger_func2();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("101"), T("hi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Cascading INSERT into UPDATE, same table",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
BEGIN
	UPDATE test SET v1 = v1 || NEW.pk::text;
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func2() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || '_u';
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                "CREATE TRIGGER test_trigger2 BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func2();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi2_u")],
                            &[T("2"), T("there")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Multiple triggers on same table",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func_a() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || 'a';
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func_c() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || 'c';
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func_b() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || 'b';
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger_b BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func_b();",
                "CREATE TRIGGER test_trigger_a BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func_a();",
                "CREATE TRIGGER test_trigger_c BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func_c();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("hiabc")],
                            &[T("2"), T("thereabc")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Stack depth limit exceeded",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE test2 (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
BEGIN
	INSERT INTO test2 VALUES (NEW.pk+2, NEW.v1 || '_');
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func2() RETURNS TRIGGER AS $$
BEGIN
	INSERT INTO test VALUES (NEW.pk+4, NEW.v1 || '|');
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test2 FOR EACH ROW EXECUTE FUNCTION trigger_func2();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Error(Diagnostic { code: "54001", message: "stack depth limit exceeded", hint: r#"Increase the configuration parameter "max_stack_depth" (currently 2048kB), after ensuring the platform's stack depth limit is adequate."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DELETE TABLE deletes attached triggers",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || '_';
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                "CREATE TRIGGER test_trigger2 BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"trigger "test_trigger" for relation "test" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER test_trigger2 BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"trigger "test_trigger2" for relation "test" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER test_trigger2 BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHEN on BEFORE INSERT",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func1() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.pk::text || '_' || NEW.v1;
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trigger_func2() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger1 BEFORE INSERT ON test FOR EACH ROW WHEN (NEW.pk < 1) EXECUTE FUNCTION trigger_func1();",
                "CREATE TRIGGER test_trigger2 BEFORE INSERT ON test FOR EACH ROW WHEN (NEW.pk > 1) EXECUTE FUNCTION trigger_func2();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 'hi'), (1, 'there'), (2, 'dude');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", TEXT)],
                        rows: &[
                            &[T("0"), T("0_hi")],
                            &[T("1"), T("there")],
                            &[T("2"), T("dude_2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: Postgres rejects the non-boolean WHEN when the trigger is created, so the creation is asserted.
        ScriptTest {
            name: "WHEN with non-boolean expression",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
BEGIN
	NEW.v1 := NEW.pk::text || '_' || NEW.v1;
	RETURN NEW;
END;
$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW WHEN (NEW.pk + 1) EXECUTE FUNCTION trigger_func();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHEN must be type boolean, not type integer", position: 70, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table as type",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, name TEXT NOT NULL, qty INT4 NOT NULL, price REAL NOT NULL);",
                r#"CREATE FUNCTION trigger_func() RETURNS trigger AS $$
DECLARE
	rec test;
BEGIN
	rec := NEW;
	IF rec.qty < 0 THEN
		rec.qty := -rec.qty;
	END IF;
	NEW := rec;
	RETURN NEW;
END; $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'apple', 3, 2.5), (2, 'banana', -5, -1.2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("qty", INT4), Column("price", FLOAT4)],
                        rows: &[
                            &[T("1"), T("apple"), T("3"), T("2.5")],
                            &[T("2"), T("banana"), T("5"), T("-1.2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DECLARE default referencing the trigger records",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, val TEXT);",
                "CREATE TABLE log (msg TEXT);",
                "INSERT INTO test VALUES (7, 'a');",
                r#"CREATE FUNCTION trigger_func() RETURNS trigger AS $$
DECLARE
	old_id INT4 := OLD.id;
	changed BOOLEAN := OLD.val <> NEW.val;
BEGIN
	INSERT INTO log VALUES ('id=' || old_id || ' changed=' || changed);
	RETURN NEW;
END; $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'b' WHERE id = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT msg FROM log;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("id=7 changed=true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "trigger to call procedure that updates another table using dynamic execute",
            set_up_script: &[
                r#"create table public."Collections"(
				 id uuid PRIMARY KEY NOT NULL,
				 name text not null,
				 username varchar(28) not null,
				 total_tracks integer DEFAULT 0);"#,
                r#"INSERT INTO public."Collections" (id, name, username, total_tracks) VALUES ('550e8400-e29b-41d4-a716-446655440000', 'My Custom Playlist', 'user_alpha', 10);"#,
                r#"create table public."CollectionItems"(
			collection_id uuid not null,
			track_id integer not null);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION update_collections()
  RETURNS trigger AS $$
  DECLARE
    BEGIN
    IF TG_OP = 'INSERT' THEN
      EXECUTE 'update public."Collections" set total_tracks=total_tracks+1 where id = $1;'
      USING NEW.collection_id;
    END IF;

    IF TG_OP = 'DELETE' THEN 
      EXECUTE 'update public."Collections" set total_tracks=total_tracks-1 where id = $1;'
      USING OLD.collection_id;
    END IF;
    
    RETURN NEW;
    END;
$$ LANGUAGE plpgsql;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER update_collection
				AFTER INSERT OR DELETE ON public."CollectionItems"
				FOR EACH ROW EXECUTE PROCEDURE update_collections();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO public."CollectionItems" (collection_id, track_id) VALUES ('550e8400-e29b-41d4-a716-446655440000', 101);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT total_tracks FROM public."Collections""#,
                    expected: Expected::Rows {
                        columns: &[Column("total_tracks", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOREACH over an array of column names",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, retired_at TEXT, note TEXT);",
                "INSERT INTO test VALUES (1, NULL, 'n');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
			DECLARE
				permitted TEXT[] := '{retired_at}';
				col TEXT;
				changed INT := 0;
			BEGIN
				FOREACH col IN ARRAY permitted LOOP
					IF col = 'retired_at' AND NEW.retired_at IS DISTINCT FROM OLD.retired_at THEN
						changed := changed + 1;
						IF OLD.retired_at IS NOT NULL THEN
							RAISE EXCEPTION 'test %: % is already set', OLD.pk, col;
						END IF;
					END IF;
				END LOOP;
				IF changed = 0 THEN
					RAISE EXCEPTION 'test % is append-only', OLD.pk;
				END IF;
				RETURN NEW;
			END;
			$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET note = 'other' WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "test 1 is append-only", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET retired_at = 'now' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET retired_at = 'later' WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "test 1: retired_at is already set", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("retired_at", TEXT), Column("note", TEXT)],
                        rows: &[
                            &[T("1"), T("now"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP TRIGGER",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					NEW.v1 := NEW.v1 || '_' || NEW.pk::text;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TRIGGER test_trigger ON test;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER IF EXISTS test_trigger ON test;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    notices: &[Diagnostic { code: "00000", message: r#"trigger "test_trigger" for relation "test" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "OLD.* IS DISTINCT FROM NEW.* in a trigger",
            set_up_script: &[
                "CREATE TABLE t3336_issue (a INT);",
                "INSERT INTO t3336_issue VALUES (1);",
                "CREATE FUNCTION f3336_issue() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'the trigger ran'; RETURN NEW; END $$;",
                "CREATE TRIGGER tr BEFORE UPDATE ON t3336_issue FOR EACH ROW WHEN (old.* IS DISTINCT FROM new.*) EXECUTE FUNCTION f3336_issue();",
                "CREATE TABLE t3336 (a INT PRIMARY KEY, b TEXT);",
                "CREATE TABLE t3336_log (a INT, src TEXT);",
                "CREATE FUNCTION f3336_when() RETURNS trigger AS $$ BEGIN INSERT INTO t3336_log VALUES (NEW.a, 'when'); RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_body() RETURNS trigger AS $$ BEGIN IF OLD.* IS DISTINCT FROM NEW.* THEN INSERT INTO t3336_log VALUES (NEW.a, 'body'); END IF; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER tr3336_when AFTER UPDATE ON t3336 FOR EACH ROW WHEN (OLD.* IS DISTINCT FROM NEW.*) EXECUTE FUNCTION f3336_when();",
                "CREATE TRIGGER tr3336_body AFTER UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_body();",
                "INSERT INTO t3336 VALUES (1, 'x'), (2, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t3336_issue SET a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    notices: &[Diagnostic { code: "00000", message: "the trigger ran", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3336_issue;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ROW(2, NULL::TEXT) IS DISTINCT FROM ROW(2, 'y'::TEXT), ROW(2, NULL::TEXT) IS DISTINCT FROM ROW(2, NULL::TEXT), ROW(2, NULL::TEXT) IS NOT DISTINCT FROM ROW(2, NULL::TEXT), ROW(1, 2) IS DISTINCT FROM ROW(1, 3), ROW(1, 2) IS NOT DISTINCT FROM ROW(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'x' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM t3336_log;",
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
                    query: "UPDATE t3336 SET b = 'y' WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = NULL WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = NULL WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'z' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3336_log ORDER BY a, src;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("src", TEXT)],
                        rows: &[
                            &[T("1"), T("body")],
                            &[T("1"), T("when")],
                            &[T("2"), T("body")],
                            &[T("2"), T("body")],
                            &[T("2"), T("when")],
                            &[T("2"), T("when")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Whole-row references outside of a record comparison are rejected",
            set_up_script: &[
                "CREATE TABLE t3336 (a INT PRIMARY KEY, b TEXT);",
                "CREATE TABLE t3336_one (a INT);",
                "CREATE TABLE t3336_bool (b BOOLEAN);",
                "CREATE TABLE t3336_log2 (v TEXT);",
                "INSERT INTO t3336 VALUES (1, 'x');",
                "INSERT INTO t3336_one VALUES (1);",
                "INSERT INTO t3336_bool VALUES (true);",
                "CREATE FUNCTION f3336() RETURNS TRIGGER AS $$ BEGIN RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_assign() RETURNS TRIGGER AS $$ DECLARE v INT; BEGIN v := NEW.*; INSERT INTO t3336_log2 VALUES ('assign ' || v); RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_if() RETURNS TRIGGER AS $$ BEGIN IF NEW.* THEN INSERT INTO t3336_log2 VALUES ('if'); END IF; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_eq() RETURNS TRIGGER AS $$ BEGIN IF NEW.* = 1 THEN RETURN NEW; END IF; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_raise() RETURNS TRIGGER AS $$ BEGIN RAISE EXCEPTION 'val %', NEW.*; END; $$ LANGUAGE plpgsql;",
                "CREATE FUNCTION f3336_ret() RETURNS TRIGGER AS $$ BEGIN RETURN NEW.*; END; $$ LANGUAGE plpgsql;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_bad BEFORE UPDATE ON t3336 FOR EACH ROW WHEN (OLD.*) EXECUTE FUNCTION f3336();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHEN must be type boolean, not type t3336", position: 69, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_bad ON t3336;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"trigger "tr3336_bad" for table "t3336" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_bad2 BEFORE UPDATE ON t3336 FOR EACH ROW WHEN (OLD.* = 1) EXECUTE FUNCTION f3336();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: t3336 = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 76, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_bad2 ON t3336;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"trigger "tr3336_bad2" for table "t3336" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_assign BEFORE UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_assign();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "assignment source returned 2 columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_assign ON t3336;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_if BEFORE UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_if();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query returned 2 columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_if ON t3336;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_eq BEFORE UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_eq();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: t3336 = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_eq ON t3336;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_raise BEFORE UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_raise();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query returned 2 columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_raise ON t3336;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_ret BEFORE UPDATE ON t3336 FOR EACH ROW EXECUTE FUNCTION f3336_ret();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336 SET b = 'q' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "query returned 2 columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_ret ON t3336;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3336;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("q")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_assign1 BEFORE UPDATE ON t3336_one FOR EACH ROW EXECUTE FUNCTION f3336_assign();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336_one SET a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_assign1 ON t3336_one;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_raise1 BEFORE UPDATE ON t3336_one FOR EACH ROW EXECUTE FUNCTION f3336_raise();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336_one SET a = 3;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "val 3", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_raise1 ON t3336_one;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3336_if1 BEFORE UPDATE ON t3336_bool FOR EACH ROW EXECUTE FUNCTION f3336_if();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336_bool SET b = true;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t3336_bool SET b = false;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tr3336_if1 ON t3336_bool;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3336_log2 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("assign 2")],
                            &[T("if")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3336_one;",
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
        ScriptTest {
            name: "UPDATE OF specific columns",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, name TEXT, data TEXT, other TEXT);",
                "INSERT INTO test VALUES (1, 'a', 'b', 'c');",
                "CREATE TABLE log (msg TEXT);",
                r#"CREATE FUNCTION trig_func1() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('tr1: ' || OLD.name || ',' || OLD.data || ',' || OLD.other || ' -> ' || NEW.name || ',' || NEW.data || ',' || NEW.other);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trig_func4() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('tr4: ' || OLD.name || ',' || OLD.data || ',' || OLD.other || ' -> ' || NEW.name || ',' || NEW.data || ',' || NEW.other);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr1 BEFORE UPDATE OF name, data ON test FOR EACH ROW EXECUTE PROCEDURE trig_func1();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr2 BEFORE UPDATE OF nope ON test FOR EACH ROW EXECUTE PROCEDURE trig_func1();",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nope" of relation "test" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr3 BEFORE UPDATE OF name, name ON test FOR EACH ROW EXECUTE PROCEDURE trig_func1();",
                    expected: Expected::Error(Diagnostic { code: "42701", message: r#"column "name" specified more than once"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tr4 AFTER INSERT OR UPDATE OF other ON test FOR EACH ROW EXECUTE PROCEDURE trig_func4();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET other = 'd';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET name = 'e';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET data = data;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log ORDER BY msg;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("tr1: a,b,d -> e,b,d")],
                            &[T("tr1: e,b,d -> e,b,d")],
                            &[T("tr4: a,b,c -> a,b,d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname, tgattr::TEXT, tgtype FROM pg_trigger WHERE tgrelid = 'test'::regclass ORDER BY tgname;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME), Column("tgattr", TEXT), Column("tgtype", INT2)],
                        rows: &[
                            &[T("tr1"), T("2 3"), T("19")],
                            &[T("tr4"), T("4"), T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE OF columns that are renamed or dropped",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, name TEXT, data TEXT, other TEXT);",
                "INSERT INTO test VALUES (1, 'a', 'b', 'c');",
                "CREATE TABLE log (msg TEXT);",
                r#"CREATE FUNCTION trig_func1() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('tr1: ' || NEW.id);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION trig_func4() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('tr4: ' || NEW.id);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER tr1 BEFORE UPDATE OF name, data ON public.test FOR EACH ROW EXECUTE FUNCTION trig_func1();",
                "CREATE TRIGGER tr4 AFTER INSERT OR UPDATE OF other ON public.test FOR EACH ROW EXECUTE FUNCTION trig_func4();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test RENAME COLUMN name TO "Name2";"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET "Name2" = 'x';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log ORDER BY msg;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("tr1: 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgrelid = 'test'::regclass ORDER BY tgname;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_triggerdef", TEXT)],
                        rows: &[
                            &[T(r#"CREATE TRIGGER tr1 BEFORE UPDATE OF "Name2", data ON public.test FOR EACH ROW EXECUTE FUNCTION trig_func1()"#)],
                            &[T("CREATE TRIGGER tr4 AFTER INSERT OR UPDATE OF other ON public.test FOR EACH ROW EXECUTE FUNCTION trig_func4()")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname, tgattr::TEXT FROM pg_trigger WHERE tgrelid = 'test'::regclass ORDER BY tgname;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME), Column("tgattr", TEXT)],
                        rows: &[
                            &[T("tr1"), T("2 3")],
                            &[T("tr4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN data;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop column data of table test because other objects depend on it", detail: "trigger tr1 on table test depends on column data of table test", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN other;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop column other of table test because other objects depend on it", detail: "trigger tr4 on table test depends on column other of table test", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN data CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to trigger tr1 on table test", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass ORDER BY tgname;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                            &[T("tr4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE OF triggers alongside an ordinary UPDATE trigger",
            set_up_script: &[
                "CREATE TABLE test (id INT4 PRIMARY KEY, name TEXT, data TEXT);",
                "INSERT INTO test VALUES (1, 'initial', 'initial');",
                "CREATE TABLE log (kind TEXT, id INT4, name TEXT, data TEXT);",
                r#"CREATE FUNCTION log_ordinary() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('ordinary', NEW.id, NEW.name, NEW.data);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION log_named_before() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('named_before', NEW.id, NEW.name, NEW.data);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION log_named_after() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log VALUES ('named_after', NEW.id, NEW.name, NEW.data);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER ordinary BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION log_ordinary();",
                "CREATE TRIGGER named_before BEFORE UPDATE OF name ON test FOR EACH ROW EXECUTE FUNCTION log_named_before();",
                "CREATE TRIGGER named_after AFTER UPDATE OF name ON test FOR EACH ROW EXECUTE FUNCTION log_named_after();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET data = 'data-only' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test AS t SET data = 'aliased' WHERE t.id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET name = name WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log ORDER BY data, kind;",
                    expected: Expected::Rows {
                        columns: &[Column("kind", TEXT), Column("id", INT4), Column("name", TEXT), Column("data", TEXT)],
                        rows: &[
                            &[T("named_after"), T("1"), T("initial"), T("aliased")],
                            &[T("named_before"), T("1"), T("initial"), T("aliased")],
                            &[T("ordinary"), T("1"), T("initial"), T("aliased")],
                            &[T("ordinary"), T("1"), T("initial"), T("aliased")],
                            &[T("ordinary"), T("1"), T("initial"), T("data-only")],
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
fn test_trigger_special_variables() {
    run_scripts(&[
        ScriptTest {
            name: "TG_ARGV assigned to a NEW column",
            set_up_script: &[
                "CREATE TABLE t (id TEXT PRIMARY KEY, seen TEXT);",
                r#"CREATE FUNCTION f_argv() RETURNS trigger LANGUAGE plpgsql AS $$
				BEGIN
					NEW.seen := TG_ARGV[0];
					RETURN NEW;
				END;
				$$;"#,
                "CREATE TRIGGER tr_argv BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION f_argv('hello', 'world');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (id) VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("seen", TEXT)],
                        rows: &[
                            &[T("a"), T("hello")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TG_ARGV subscripts and slices count from 0",
            set_up_script: &[
                "CREATE TABLE t (id TEXT PRIMARY KEY);",
                "CREATE TABLE log (v TEXT);",
                r#"CREATE FUNCTION f_argv() RETURNS trigger LANGUAGE plpgsql AS $$
				DECLARE
					i INT := 0;
				BEGIN
					INSERT INTO log (v) VALUES (concat_ws(',', TG_NARGS, TG_ARGV[0], TG_ARGV[1],
						coalesce(TG_ARGV[2], 'none'), coalesce(TG_ARGV[-1], 'none'), tg_argv[i + 1], TG_ARGV[0:1]::text, TG_ARGV[:0]::text, TG_ARGV[1:]::text,
						array_length(TG_ARGV, 1)));
					RETURN NULL;
				END;
				$$;"#,
                "CREATE TRIGGER tr_args AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION f_argv('x', 'y');",
                "CREATE TRIGGER tr_no_args AFTER DELETE ON t FOR EACH ROW EXECUTE FUNCTION f_argv();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM log ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("0,none,none")],
                            &[T("2,x,y,none,none,y,{x,y},{x},{y},2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TG_ARGV keeps its lower bound of 0",
            set_up_script: &[
                "CREATE TABLE t (id TEXT PRIMARY KEY);",
                "CREATE TABLE log (v TEXT);",
                r#"CREATE FUNCTION f_argv() RETURNS trigger LANGUAGE plpgsql AS $$
				BEGIN
					INSERT INTO log (v) VALUES (TG_ARGV::text);
					RETURN NULL;
				END;
				$$;"#,
                "CREATE TRIGGER tr_args AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION f_argv('x', 'y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM log ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("[0:1]={x,y}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TG_OP and TG_WHEN in BEFORE triggers",
            set_up_script: &[
                "CREATE TABLE t (id TEXT PRIMARY KEY, seen TEXT);",
                "CREATE TABLE log (v TEXT);",
                r#"CREATE FUNCTION f_before() RETURNS trigger LANGUAGE plpgsql AS $$
				BEGIN
					IF TG_OP = 'DELETE' THEN
						INSERT INTO log (v) VALUES (TG_WHEN || ' ' || TG_OP || ' ' || OLD.id);
						RETURN OLD;
					END IF;
					NEW.seen := TG_WHEN || ' ' || TG_OP;
					RETURN NEW;
				END;
				$$;"#,
                "CREATE TRIGGER tr_before BEFORE INSERT OR UPDATE OR DELETE ON t FOR EACH ROW EXECUTE FUNCTION f_before();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (id) VALUES ('a'), ('b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET id = 'c' WHERE id = 'b';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("seen", TEXT)],
                        rows: &[
                            &[T("a"), T("BEFORE INSERT")],
                            &[T("c"), T("BEFORE UPDATE")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id = 'a';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM log ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("BEFORE DELETE a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "trigger and table names",
            set_up_script: &[
                "CREATE SCHEMA s;",
                "CREATE TABLE s.t (id TEXT PRIMARY KEY);",
                "CREATE TABLE log (v TEXT);",
                r#"CREATE FUNCTION f_after() RETURNS trigger LANGUAGE plpgsql AS $$
				BEGIN
					INSERT INTO public.log (v) VALUES (concat_ws(',', TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, TG_TABLE_NAME,
						TG_TABLE_SCHEMA, TG_RELNAME, TG_RELID::regclass::text));
					RETURN NULL;
				END;
				$$;"#,
                "CREATE TRIGGER tr_after AFTER INSERT ON s.t FOR EACH ROW EXECUTE FUNCTION f_after();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO s.t VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM log ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("tr_after,AFTER,ROW,INSERT,t,s,t,s.t")],
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
fn test_trigger_whole_record_reference() {
    run_scripts(&[
        ScriptTest {
            name: "whole record passed to a function",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, which TEXT, j JSONB);",
                "INSERT INTO test VALUES (1, 'hi');",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				DECLARE
					old_v jsonb;
					new_v jsonb;
				BEGIN
					old_v := to_jsonb(OLD);
					new_v := to_jsonb(NEW);
					INSERT INTO log (which, j) VALUES ('old', old_v), ('new', new_v);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = 'bye' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT which, j::text FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("which", TEXT), Column("j", TEXT)],
                        rows: &[
                            &[T("old"), T(r#"{"pk": 1, "v1": "hi"}"#)],
                            &[T("new"), T(r#"{"pk": 1, "v1": "bye"}"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "whole record as text",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT, b BOOL);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, t TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log (t) VALUES (NEW::text);
					RAISE NOTICE 'row: %', NEW;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1, 'a,b"c', true);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    notices: &[Diagnostic { code: "00000", message: r#"row: (1,"a,b""c",t)"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT)],
                        rows: &[
                            &[T(r#"(1,"a,b""c",t)"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "whole record with a NULL field",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, j TEXT, t TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log (j, t) VALUES (to_jsonb(NEW)::text, NEW::text);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT j, t FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("j", TEXT), Column("t", TEXT)],
                        rows: &[
                            &[T(r#"{"pk": 1, "v1": null}"#), T("(1,)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "whole record whose field is named like the record",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, record TEXT);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, j TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log (j) VALUES (to_jsonb(NEW)::text);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT j FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("j", TEXT)],
                        rows: &[
                            &[T(r#"{"pk": 1, "record": "hi"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "records compared as a whole",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "INSERT INTO test VALUES (1, 'hi'), (2, 'there');",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, msg TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					IF NEW IS DISTINCT FROM OLD THEN
						INSERT INTO log (msg) VALUES ('changed ' || NEW.pk::text);
					ELSE
						INSERT INTO log (msg) VALUES ('same ' || NEW.pk::text);
					END IF;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = 'hi' WHERE pk IN (1, 2);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT msg FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("same 1")],
                            &[T("changed 2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "records compared as a whole when a field leaves NULL",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT, v2 TEXT);",
                "INSERT INTO test VALUES (1, 'hi', NULL);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, msg TEXT);",
                r#"CREATE FUNCTION trigger_func() RETURNS TRIGGER AS $$
				BEGIN
					IF OLD IS DISTINCT FROM NEW THEN
						INSERT INTO log (msg) VALUES ('changed ' || NEW.pk::text);
					ELSE
						INSERT INTO log (msg) VALUES ('same ' || NEW.pk::text);
					END IF;
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER test_trigger BEFORE UPDATE ON test FOR EACH ROW EXECUTE FUNCTION trigger_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = 'bye' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v2 = 'now set' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v2 = NULL WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v2 = NULL WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT msg FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("changed 1")],
                            &[T("changed 1")],
                            &[T("changed 1")],
                            &[T("same 1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "the record an operation does not supply",
            set_up_script: &[
                "CREATE TABLE test (pk INT PRIMARY KEY, v1 TEXT);",
                "CREATE TABLE log (id SERIAL PRIMARY KEY, o TEXT, n TEXT);",
                r#"CREATE FUNCTION insert_func() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log (o, n) VALUES (to_jsonb(OLD)::text, to_jsonb(NEW)::text);
					RETURN NEW;
				END;
				$$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION delete_func() RETURNS TRIGGER AS $$
				BEGIN
					INSERT INTO log (o, n) VALUES (to_jsonb(OLD)::text, to_jsonb(NEW)::text);
					RETURN OLD;
				END;
				$$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER t1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION insert_func();",
                "CREATE TRIGGER t2 BEFORE DELETE ON test FOR EACH ROW EXECUTE FUNCTION delete_func();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'hi');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT o, n FROM log ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("o", TEXT), Column("n", TEXT)],
                        rows: &[
                            &[Null, T(r#"{"pk": 1, "v1": "hi"}"#)],
                            &[T(r#"{"pk": 1, "v1": "hi"}"#), Null],
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
fn test_trigger_rules() {
    run_scripts(&[
        ScriptTest {
            name: "row and statement triggers",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (id int PRIMARY KEY, v text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE audit (n serial, what text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION before_row() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF TG_OP = 'DELETE' THEN INSERT INTO audit (what) VALUES ('delete ' || OLD.id); RETURN OLD; END IF; NEW.v := upper(NEW.v); IF NEW.v = 'SKIP' THEN RETURN NULL; END IF; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION after_row() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit (what) VALUES (TG_WHEN || ' ' || TG_LEVEL || ' ' || TG_OP || ' ' || TG_TABLE_NAME || ' ' || coalesce(NEW.v, OLD.v)); RETURN NULL; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION per_statement() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO audit (what) VALUES (TG_WHEN || ' ' || TG_LEVEL || ' ' || TG_OP || ' ' || TG_NARGS || ' ' || TG_ARGV[0]); RETURN NULL; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER b BEFORE INSERT OR UPDATE OR DELETE ON t FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER a AFTER INSERT OR UPDATE ON t FOR EACH ROW EXECUTE FUNCTION after_row();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s AFTER INSERT ON t FOR EACH STATEMENT EXECUTE FUNCTION per_statement('arg', 'two');",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 'one'), (2, 'skip'), (3, 'three');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("ONE")],
                            &[T("3"), T("THREE")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 'uno' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("UNO")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT what FROM audit ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT)],
                        rows: &[
                            &[T("AFTER ROW INSERT t ONE")],
                            &[T("AFTER ROW INSERT t THREE")],
                            &[T("AFTER STATEMENT INSERT 2 arg")],
                            &[T("AFTER ROW UPDATE t UNO")],
                            &[T("delete 3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER b BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"trigger "b" for relation "t" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x BEFORE INSERT ON nope FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nope" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION nope();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nope() does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION not_trigger() RETURNS int LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION not_trigger();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "function not_trigger must return type trigger", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x INSTEAD OF INSERT ON t FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t" is a table"#, detail: "Tables cannot have INSTEAD OF triggers.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x BEFORE UPDATE OF nope ON t FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nope" of relation "t" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER x BEFORE UPDATE OF v, v ON t FOR EACH ROW EXECUTE FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "42701", message: r#"column "v" specified more than once"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION before_row();",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop function before_row() because other objects depend on it", detail: "trigger b on table t depends on function before_row()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER nope ON t;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"trigger "nope" for table "t" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER IF EXISTS nope ON t;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    notices: &[Diagnostic { code: "00000", message: r#"trigger "nope" for relation "t" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER b ON t;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER a ON t;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER s ON t;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION before_row();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "trigger WHEN conditions and UPDATE OF columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (id int PRIMARY KEY, a int, b int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE seen (what text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION note() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO seen VALUES (TG_NAME || ':' || NEW.id); RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER big BEFORE INSERT ON t FOR EACH ROW WHEN (NEW.a > 10) EXECUTE FUNCTION note();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER changed AFTER UPDATE ON t FOR EACH ROW WHEN (OLD.a IS DISTINCT FROM NEW.a) EXECUTE FUNCTION note();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER of_b AFTER UPDATE OF b ON t FOR EACH ROW EXECUTE FUNCTION note();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 5, 0), (2, 50, 0);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a + 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET b = 1 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT what FROM seen ORDER BY what;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT)],
                        rows: &[
                            &[T("big:2")],
                            &[T("changed:1")],
                            &[T("of_b:2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER bad BEFORE INSERT ON t FOR EACH ROW WHEN (NEW.a + 1) EXECUTE FUNCTION note();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHEN must be type boolean, not type integer", position: 58, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER bad BEFORE DELETE ON t FOR EACH ROW WHEN (NEW.a > 1) EXECUTE FUNCTION note();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "DELETE trigger's WHEN condition cannot reference NEW values", position: 58, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER bad BEFORE INSERT ON t FOR EACH ROW WHEN (OLD.a > 1) EXECUTE FUNCTION note();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "INSERT trigger's WHEN condition cannot reference OLD values", position: 58, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER bad AFTER INSERT ON t FOR EACH STATEMENT WHEN (NEW.a > 1) EXECUTE FUNCTION note();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "statement trigger's WHEN condition cannot reference column values", position: 63, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_builtin_trigger_functions() {
    run_scripts(&[
        ScriptTest {
            name: "suppress_redundant_updates_trigger skips updates that change nothing",
            set_up_script: &[
                "CREATE TABLE s (a INT PRIMARY KEY, b TEXT);",
                "CREATE TRIGGER s1 BEFORE UPDATE ON s FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                "INSERT INTO s VALUES (1, 'a'), (2, 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = 'c' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[],
                        tag: "UPDATE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("c")], &[T("2"), T("b")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHEN conditions, disabled triggers, and replica triggers",
            set_up_script: &[
                "CREATE TABLE s (a INT PRIMARY KEY, b TEXT);",
                "CREATE TRIGGER s1 BEFORE UPDATE ON s FOR EACH ROW WHEN (OLD.a = 2) EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                "INSERT INTO s VALUES (1, 'a'), (2, 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s DISABLE TRIGGER s1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s ENABLE REPLICA TRIGGER s1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = b;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "suppress_redundant_updates_trigger refuses other events, timings, and levels",
            set_up_script: &[
                "CREATE TABLE s (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO s VALUES (1, 'a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s1 BEFORE INSERT ON s FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s VALUES (3, 'x');",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called on update", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER s1 ON s;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s2 AFTER UPDATE ON s FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = 'd';",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called before update", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER s2 ON s;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s3 BEFORE UPDATE ON s FOR EACH STATEMENT EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE s SET b = 'd';",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called for each row", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER s3 ON s;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s4 BEFORE TRUNCATE ON s FOR EACH STATEMENT EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE s;",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called on update", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER s4 ON s;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER s5 BEFORE DELETE ON s FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM s;",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called on update", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("a")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "trigger functions called from SQL, and pg_trigger_depth",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT suppress_redundant_updates_trigger();",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called as trigger", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "RI_FKey_check_ins"();"#,
                    expected: Expected::Error(Diagnostic { code: "39P01", message: r#"function "RI_FKey_check_ins" was not called by trigger manager"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique_key_recheck();",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: r#"function "unique_key_recheck" was not called by trigger manager"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsvector_update_trigger();",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: not fired by trigger manager", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_trigger_depth();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_trigger_depth", INT4)],
                        rows: &[&[T("0")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TRUNCATE of a table with pending trigger events",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY);",
                "CREATE TABLE d (a INT REFERENCES p DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE d;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot TRUNCATE "d" because it has pending trigger events"#, ..E }),
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
            name: "INSTEAD OF UPDATE and DELETE triggers on views receive the view's rows",
            set_up_script: &[
                "CREATE TABLE b (a INT, t TEXT);",
                "INSERT INTO b VALUES (1, 'x'), (2, 'y');",
                "CREATE VIEW v AS SELECT a, t FROM b GROUP BY a, t;",
                "CREATE TRIGGER vu INSTEAD OF UPDATE ON v FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                "CREATE TRIGGER vd INSTEAD OF DELETE ON v FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE v SET t = 'z' WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called before update", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v SET t = 'z' WHERE a = 5;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM v WHERE a = 2;",
                    expected: Expected::Error(Diagnostic { code: "39P01", message: "suppress_redundant_updates_trigger: must be called on update", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_trigger_executor() {
    run_scripts(&[
        ScriptTest {
            name: "statement and row triggers fire in order, statement triggers once",
            set_up_script: &[
                "CREATE TABLE log (n SERIAL PRIMARY KEY, what TEXT);",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                r#"CREATE FUNCTION note() RETURNS TRIGGER AS $$
BEGIN
  INSERT INTO log (what) VALUES (TG_NAME || ':' || TG_WHEN || ':' || TG_LEVEL || ':' || TG_OP || ':' || pg_trigger_depth());
  IF TG_LEVEL = 'ROW' THEN
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
  END IF;
  RETURN NULL;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER bs BEFORE INSERT OR UPDATE OR DELETE ON t FOR EACH STATEMENT EXECUTE FUNCTION note();",
                "CREATE TRIGGER br BEFORE INSERT OR UPDATE OR DELETE ON t FOR EACH ROW EXECUTE FUNCTION note();",
                "CREATE TRIGGER ar AFTER INSERT OR UPDATE OR DELETE ON t FOR EACH ROW EXECUTE FUNCTION note();",
                "CREATE TRIGGER as_ AFTER INSERT OR UPDATE OR DELETE ON t FOR EACH STATEMENT EXECUTE FUNCTION note();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 10), (2, 20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = v + 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id = 3;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT what FROM log ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT)],
                        rows: &[
                            &[T("bs:BEFORE:STATEMENT:INSERT:1")],
                            &[T("br:BEFORE:ROW:INSERT:1")],
                            &[T("br:BEFORE:ROW:INSERT:1")],
                            &[T("ar:AFTER:ROW:INSERT:1")],
                            &[T("ar:AFTER:ROW:INSERT:1")],
                            &[T("as_:AFTER:STATEMENT:INSERT:1")],
                            &[T("bs:BEFORE:STATEMENT:UPDATE:1")],
                            &[T("br:BEFORE:ROW:UPDATE:1")],
                            &[T("ar:AFTER:ROW:UPDATE:1")],
                            &[T("as_:AFTER:STATEMENT:UPDATE:1")],
                            &[T("bs:BEFORE:STATEMENT:DELETE:1")],
                            &[T("as_:AFTER:STATEMENT:DELETE:1")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "transition tables",
            set_up_script: &[
                "CREATE TABLE log (n SERIAL PRIMARY KEY, what TEXT);",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                r#"CREATE FUNCTION counts() RETURNS TRIGGER AS $$
DECLARE o INT := 0; nw INT := 0;
BEGIN
  IF TG_OP IN ('UPDATE', 'DELETE') THEN SELECT count(*) INTO o FROM old_rows; END IF;
  IF TG_OP IN ('INSERT', 'UPDATE') THEN SELECT count(*) INTO nw FROM new_rows; END IF;
  INSERT INTO log (what) VALUES (TG_OP || ' old=' || o || ' new=' || nw);
  RETURN NULL;
END $$ LANGUAGE plpgsql;"#,
                r#"CREATE FUNCTION sums() RETURNS TRIGGER AS $$
DECLARE s INT;
BEGIN
  SELECT sum(n.v - o.v) INTO s FROM new_rows n JOIN old_rows o USING (id);
  INSERT INTO log (what) VALUES ('delta ' || s);
  RETURN NULL;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER ins AFTER INSERT ON t REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION counts();",
                "CREATE TRIGGER upd AFTER UPDATE ON t REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION counts();",
                "CREATE TRIGGER del AFTER DELETE ON t REFERENCING OLD TABLE AS old_rows FOR EACH STATEMENT EXECUTE FUNCTION counts();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = v + 1 WHERE id > 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER rowsum AFTER UPDATE ON t REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows FOR EACH ROW EXECUTE FUNCTION sums();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = v * 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT what FROM log ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT)],
                        rows: &[
                            &[T("INSERT old=0 new=3")],
                            &[T("UPDATE old=2 new=2")],
                            &[T("DELETE old=1 new=0")],
                            &[T("delta 31")],
                            &[T("delta 31")],
                            &[T("UPDATE old=2 new=2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER bad BEFORE UPDATE ON t REFERENCING OLD TABLE AS o FOR EACH ROW EXECUTE FUNCTION sums();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "transition table name can only be specified for an AFTER trigger", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BEFORE ROW triggers change and skip rows, in INSERT, UPDATE, and COPY",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                r#"CREATE FUNCTION fix() RETURNS TRIGGER AS $$
BEGIN
  IF NEW.v < 0 THEN RETURN NULL; END IF;
  NEW.v := NEW.v * 10;
  RETURN NEW;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER fix BEFORE INSERT OR UPDATE ON t FOR EACH ROW EXECUTE FUNCTION fix();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1), (2, -1), (3, 3) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[&[T("1"), T("10")], &[T("3"), T("30")]],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = -5 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 4 WHERE id = 3 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[&[T("3"), T("40")]],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "trigger-copy-rows.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[&[T("1"), T("10")], &[T("3"), T("40")], &[T("4"), T("20")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSTEAD OF INSERT triggers on views, and TRUNCATE triggers",
            set_up_script: &[
                "CREATE TABLE log (n SERIAL PRIMARY KEY, what TEXT);",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 10), (3, 40), (4, 20);",
                "CREATE VIEW agg AS SELECT count(*) AS c FROM t;",
                r#"CREATE FUNCTION viewins() RETURNS TRIGGER AS $$
BEGIN
  INSERT INTO log (what) VALUES ('instead ' || NEW.c);
  IF NEW.c = 0 THEN RETURN NULL; END IF;
  RETURN NEW;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER vi INSTEAD OF INSERT ON agg FOR EACH ROW EXECUTE FUNCTION viewins();",
                r#"CREATE FUNCTION tr() RETURNS TRIGGER AS $$
BEGIN
  INSERT INTO log (what) VALUES (TG_NAME || ' ' || TG_WHEN || ' ' || TG_OP || ' ' || (SELECT count(*) FROM t));
  RETURN NULL;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER tb BEFORE TRUNCATE ON t FOR EACH STATEMENT EXECUTE FUNCTION tr();",
                "CREATE TRIGGER ta AFTER TRUNCATE ON t FOR EACH STATEMENT EXECUTE FUNCTION tr();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO agg VALUES (7), (0) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("c", INT8)],
                        rows: &[&[T("7")]],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY agg FROM STDIN;",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "trigger-copy-view.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE t;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT what FROM log ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT)],
                        rows: &[
                            &[T("instead 7")],
                            &[T("instead 0")],
                            &[T("instead 8")],
                            &[T("instead 0")],
                            &[T("tb BEFORE TRUNCATE 3")],
                            &[T("ta AFTER TRUNCATE 0")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a cloned BEFORE ROW trigger may not move a row to another partition",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "CREATE FUNCTION bump() RETURNS TRIGGER AS $$ BEGIN NEW.a := NEW.a + 10; RETURN NEW; END $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER bump BEFORE INSERT ON pt FOR EACH ROW EXECUTE FUNCTION bump();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (1, 'x');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "moving row to another partition during a BEFORE FOR EACH ROW trigger is not supported", detail: r#"Before executing trigger "bump", the row was to be in partition "public.pt1"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt1 VALUES (1, 'x');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "moving row to another partition during a BEFORE FOR EACH ROW trigger is not supported", detail: r#"Before executing trigger "bump", the row was to be in partition "public.pt1"."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSTEAD OF UPDATE and DELETE triggers on views change the base table",
            set_up_script: &[
                "CREATE TABLE b (a INT PRIMARY KEY, t TEXT);",
                "INSERT INTO b VALUES (1, 'x'), (2, 'y');",
                "CREATE VIEW v AS SELECT a, t FROM b GROUP BY a, t;",
                r#"CREATE FUNCTION vupd() RETURNS TRIGGER AS $$
BEGIN
  IF TG_OP = 'UPDATE' THEN
    UPDATE b SET t = NEW.t WHERE a = OLD.a;
    RETURN NEW;
  END IF;
  DELETE FROM b WHERE a = OLD.a;
  RETURN OLD;
END $$ LANGUAGE plpgsql;"#,
                "CREATE TRIGGER vu INSTEAD OF UPDATE OR DELETE ON v FOR EACH ROW EXECUTE FUNCTION vupd();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE v SET t = 'z' WHERE a = 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("t", TEXT)],
                        rows: &[&[T("1"), T("z")]],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM v WHERE a = 2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("t", TEXT)],
                        rows: &[&[T("2"), T("y")]],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM b ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("t", TEXT)],
                        rows: &[&[T("1"), T("z")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a trigger function's queries read its transition tables, which SQL functions it calls cannot see",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, n INT);",
                "CREATE TABLE log (what TEXT, total BIGINT);",
                "CREATE FUNCTION sum_new() RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO log SELECT 'new', sum(n) FROM new_table;
  IF TG_OP = 'UPDATE' THEN
    INSERT INTO log SELECT 'old', sum(n) FROM old_table;
  END IF;
  RETURN NULL;
END $$;",
                "CREATE TRIGGER s_ins AFTER INSERT ON t REFERENCING NEW TABLE AS new_table FOR EACH STATEMENT EXECUTE FUNCTION sum_new();",
                "CREATE TRIGGER s_upd AFTER UPDATE ON t REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table FOR EACH STATEMENT EXECUTE FUNCTION sum_new();",
                "SET check_function_bodies = off;",
                "CREATE FUNCTION count_new() RETURNS BIGINT LANGUAGE sql AS $$ SELECT count(*) FROM new_table $$;",
                "RESET check_function_bodies;",
                "CREATE FUNCTION calls_sql() RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
  PERFORM count_new();
  RETURN NULL;
END $$;",
                "CREATE TABLE u (id INT);",
                "CREATE TRIGGER u_ins AFTER INSERT ON u REFERENCING NEW TABLE AS new_table FOR EACH STATEMENT EXECUTE FUNCTION calls_sql();",
                "CREATE FUNCTION modifies() RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
  DELETE FROM new_table;
  RETURN NULL;
END $$;",
                "CREATE TABLE w (id INT);",
                "CREATE TRIGGER w_ins AFTER INSERT ON w REFERENCING NEW TABLE AS new_table FOR EACH STATEMENT EXECUTE FUNCTION modifies();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 10), (2, 20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET n = n + 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log ORDER BY what, total;",
                    expected: Expected::Rows {
                        columns: &[Column("what", TEXT), Column("total", INT8)],
                        rows: &[
                            &[T("new"), T("30")],
                            &[T("new"), T("32")],
                            &[T("old"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "new_table" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"relation "new_table" cannot be the target of a modifying statement"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_trigger_functions() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL trigger functions read the TG variables, NEW, and OLD, and a WHEN condition limits them",
            set_up_script: &[
                "CREATE TABLE tg (id INT PRIMARY KEY, v TEXT);",
                "CREATE TABLE tlog (n SERIAL, msg TEXT);",
                "CREATE FUNCTION tinfo() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO tlog (msg) VALUES (concat_ws(' ', TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_RELID = 'tg'::regclass, TG_NARGS, NEW.v, OLD.v)); IF TG_LEVEL = 'ROW' AND TG_OP <> 'DELETE' THEN RETURN NEW; END IF; RETURN OLD; END $$;",
                "CREATE TRIGGER a_bri BEFORE INSERT ON tg FOR EACH ROW EXECUTE FUNCTION tinfo('x', 'y');",
                "CREATE TRIGGER b_asu AFTER UPDATE ON tg FOR EACH STATEMENT EXECUTE FUNCTION tinfo();",
                "CREATE TRIGGER c_ard AFTER DELETE ON tg FOR EACH ROW WHEN (OLD.id > 1) EXECUTE FUNCTION tinfo();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO tg VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tg SET v = v || '!';",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM tg;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT msg FROM tlog ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("msg", TEXT)],
                        rows: &[
                            &[T("a_bri BEFORE ROW INSERT tg public t 2 one")],
                            &[T("a_bri BEFORE ROW INSERT tg public t 2 two")],
                            &[T("b_asu AFTER STATEMENT UPDATE tg public t 0")],
                            &[T("c_ard AFTER ROW DELETE tg public t 0 two!")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a PL/pgSQL statement trigger reads its transition table",
            set_up_script: &[
                "CREATE TABLE tt2 (a INT);",
                "CREATE FUNCTION trans() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'new rows %', (SELECT string_agg(a::text, ',' ORDER BY a) FROM newtab); RETURN NULL; END $$;",
                "CREATE TRIGGER t_trans AFTER INSERT ON tt2 REFERENCING NEW TABLE AS newtab FOR EACH STATEMENT EXECUTE FUNCTION trans();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO tt2 VALUES (3), (1), (2);",
                    expected: Expected::Tag("INSERT 0 3"),
                    notices: &[Diagnostic { code: "00000", message: "new rows 1,2,3", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PL/pgSQL trigger functions fail for a returned row of another structure, no RETURN, and a raised error",
            set_up_script: &[
                "CREATE FUNCTION badret() RETURNS trigger LANGUAGE plpgsql AS $$ DECLARE r RECORD; BEGIN SELECT 1 AS x INTO r; RETURN r; END $$;",
                "CREATE TABLE tb (a INT, b TEXT);",
                "CREATE TRIGGER t_bad BEFORE INSERT ON tb FOR EACH ROW EXECUTE FUNCTION badret();",
                "CREATE FUNCTION noret() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NULL; END $$;",
                "CREATE TABLE tn (a INT);",
                "CREATE TRIGGER t_noret BEFORE INSERT ON tn FOR EACH ROW EXECUTE FUNCTION noret();",
                "CREATE FUNCTION boom() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'no % allowed', NEW.a; END $$;",
                "CREATE TABLE te (a INT);",
                "CREATE TRIGGER t_boom BEFORE INSERT ON te FOR EACH ROW EXECUTE FUNCTION boom();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO tb VALUES (1, 'x');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "returned row structure does not match the structure of the triggering table", detail: "Number of returned columns (1) does not match expected column count (2).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tn VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "2F005", message: "control reached end of trigger procedure without RETURN", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO te VALUES (7);",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "no 7 allowed", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSTEAD OF row triggers written in PL/pgSQL change a view's table",
            set_up_script: &[
                "CREATE TABLE base (id INT, v TEXT);",
                "CREATE VIEW bv AS SELECT * FROM base;",
                "CREATE FUNCTION bv_ins() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF TG_OP = 'INSERT' THEN INSERT INTO base VALUES (NEW.id, upper(NEW.v)); RETURN NEW; ELSIF TG_OP = 'UPDATE' THEN UPDATE base SET v = NEW.v WHERE id = OLD.id; RETURN NEW; ELSE DELETE FROM base WHERE id = OLD.id; RETURN OLD; END IF; END $$;",
                "CREATE TRIGGER bv_io INSTEAD OF INSERT OR UPDATE OR DELETE ON bv FOR EACH ROW EXECUTE FUNCTION bv_ins();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO bv VALUES (1, 'a'), (2, 'b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE bv SET v = 'z' WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM bv WHERE id = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM base ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("z")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a PL/pgSQL TRUNCATE trigger fires for the statement",
            set_up_script: &[
                "CREATE TABLE ttr (a INT);",
                "CREATE FUNCTION trunc_log() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% % %', TG_OP, TG_WHEN, TG_LEVEL; RETURN NULL; END $$;",
                "CREATE TRIGGER t_trunc BEFORE TRUNCATE ON ttr FOR EACH STATEMENT EXECUTE FUNCTION trunc_log();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "TRUNCATE ttr;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "TRUNCATE BEFORE STATEMENT", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a partitioned table's PL/pgSQL trigger is cloned to its partitions and a row moving between them fires delete and insert triggers",
            set_up_script: &[
                "CREATE TABLE pt (k INT, v TEXT) PARTITION BY RANGE (k);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "CREATE FUNCTION pt_log() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% % on % (%)', TG_WHEN, TG_OP, TG_TABLE_NAME, coalesce(NEW.k, OLD.k); RETURN coalesce(NEW, OLD); END $$;",
                "CREATE TRIGGER pt_b BEFORE INSERT OR UPDATE OR DELETE ON pt FOR EACH ROW EXECUTE FUNCTION pt_log();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgname FROM pg_trigger WHERE tgname = 'pt_b' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgname", NAME)],
                        rows: &[
                            &[T("pt"), T("pt_b")],
                            &[T("pt1"), T("pt_b")],
                            &[T("pt2"), T("pt_b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (5, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    notices: &[Diagnostic { code: "00000", message: "BEFORE INSERT on pt1 (5)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET k = 15 WHERE k = 5;",
                    expected: Expected::Tag("UPDATE 1"),
                    notices: &[Diagnostic { code: "00000", message: "BEFORE UPDATE on pt1 (15)", ..N }, Diagnostic { code: "00000", message: "BEFORE DELETE on pt1 (5)", ..N }, Diagnostic { code: "00000", message: "BEFORE INSERT on pt2 (15)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("k", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("pt2"), T("15"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a deferred PL/pgSQL constraint trigger queued in a rolled-back exception block does not fire at commit",
            set_up_script: &[
                "CREATE TABLE ct (a INT);",
                "CREATE FUNCTION ct_note() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'fired for %', NEW.a; RETURN NULL; END $$;",
                "CREATE CONSTRAINT TRIGGER ct_def AFTER INSERT ON ct DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION ct_note();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ct VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN INSERT INTO ct VALUES (2); RAISE EXCEPTION 'boom'; EXCEPTION WHEN OTHERS THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { code: "00000", message: "fired for 1", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_of_transition_tables_under_execute() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN run by EXECUTE in a trigger function reads the trigger's transition tables",
            set_up_script: &[
                "CREATE TABLE ttb (id INT, val TEXT);",
                "CREATE FUNCTION tt_fn() RETURNS trigger LANGUAGE plpgsql AS $$ DECLARE l TEXT; t TEXT := ''; BEGIN FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM newtable' LOOP t := t || l; END LOOP; RAISE INFO '%', t; RETURN NULL; END $$;",
                "CREATE TRIGGER tt_trig AFTER INSERT ON ttb REFERENCING NEW TABLE AS newtable FOR EACH STATEMENT EXECUTE PROCEDURE tt_fn();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO ttb VALUES (1, 'a'), (2, 'b');",
                    expected: Expected::Tag("INSERT 0 2"),
                    notices: &[Diagnostic { severity: "INFO", code: "00000", message: "Named Tuplestore Scan (actual rows=2.00 loops=1)", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_when_conditions_over_tableoid() {
    run_scripts(&[
        ScriptTest {
            name: "a trigger's WHEN condition reads the table OID of OLD and NEW",
            set_up_script: &[
                "CREATE TABLE table_with_oids (a INT);",
                "INSERT INTO table_with_oids VALUES (1);",
                "CREATE FUNCTION tf() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'fired %', TG_NAME; RETURN NULL; END $$;",
                "CREATE TRIGGER oid_unchanged_trig AFTER UPDATE ON table_with_oids FOR EACH ROW WHEN (new.tableoid = old.tableoid AND new.tableoid <> 0) EXECUTE PROCEDURE tf();",
                "CREATE TRIGGER old_oid_trig AFTER DELETE ON table_with_oids FOR EACH ROW WHEN (old.tableoid = 'table_with_oids'::regclass) EXECUTE PROCEDURE tf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE table_with_oids SET a = a + 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    notices: &[Diagnostic { code: "00000", message: "fired oid_unchanged_trig", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM table_with_oids;",
                    expected: Expected::Tag("DELETE 1"),
                    notices: &[Diagnostic { code: "00000", message: "fired old_oid_trig", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}
