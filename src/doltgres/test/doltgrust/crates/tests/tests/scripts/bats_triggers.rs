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

//! Dolt's integration-tests/bats/triggers.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_triggers() {
    run_scripts(&[
        ScriptTest {
            name: r#"triggers: Mix of CREATE TRIGGER and CREATE VIEW in batch mode"#,
            intent: r#"Triggers and views created in one batch all work: each trigger fires on its table's inserts, and each view reads its table."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"CREATE FUNCTION trigger1_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.v1 := -NEW.v1; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger1_fn();"#,
                r#"CREATE VIEW view1 AS SELECT v1 FROM test;"#,
                r#"CREATE TABLE a (x INT PRIMARY KEY);"#,
                r#"CREATE TABLE b (y INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
                r#"CREATE VIEW view2 AS SELECT y FROM b;"#,
                r#"CREATE FUNCTION trigger2_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN INSERT INTO b VALUES (NEW.x * 2); RETURN NULL; END$$;"#,
                r#"CREATE TRIGGER trigger2 AFTER INSERT ON a FOR EACH ROW EXECUTE FUNCTION trigger2_fn();"#,
                r#"INSERT INTO a VALUES (2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM b;"#,
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM view1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM view2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tgname FROM pg_trigger WHERE tgrelid IN ('test'::regclass, 'a'::regclass) ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                            &[T("trigger1")],
                            &[T("trigger2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust keeps views and triggers as schema objects of their own (STORAGE_DESIGN IV.3a), so pg_trigger and pg_views list them, where Dolt keeps them as rows of dolt_schemas")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("viewname", NAME)],
                        rows: &[
                            &[T("view1")],
                            &[T("view2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust keeps views and triggers as schema objects of their own (STORAGE_DESIGN IV.3a), so pg_trigger and pg_views list them, where Dolt keeps them as rows of dolt_schemas")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"triggers: Order preservation"#,
            intent: r#"Triggers fire in the order PRECEDES and FOLLOWS set."#,
            set_up_script: &[
                r#"CREATE TABLE x (a BIGINT PRIMARY KEY);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("21040")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some(r#"MySQL-only: FOLLOWS and PRECEDES order MySQL's triggers, where Postgres fires a table's triggers in name order"#),
            ..S
        },
        ScriptTest {
            name: r#"triggers: Writing directly into dolt_schemas is forbidden"#,
            intent: r#"A trigger can't be made by writing its definition as a row."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"CREATE VIEW view1 AS SELECT v1 FROM test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_schemas VALUES ('trigger', 'trigger1', 'CREATE TRIGGER trigger1 BEFORE INSERT ON test FOR EACH ROW SET new.v1 = -new.v1;', '{}');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "dolt_schemas" does not exist"#, position: 13, ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust keeps views and triggers as schema objects of their own (STORAGE_DESIGN IV.3a), so there is no dolt_schemas to write")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"triggers: Merge triggers on different branches, no conflict"#,
            intent: r#"Triggers added on two branches merge without conflict, and the diff between the branches shows each side's trigger."#,
            set_up_script: &[
                r#"CREATE TABLE x(a BIGINT PRIMARY KEY);"#,
                r#"CREATE FUNCTION trigger1_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.a := NEW.a + 1; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger1 BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION trigger1_fn();"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'Initial Commit');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"CREATE FUNCTION trigger2_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.a := (NEW.a * 2) + 10; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger2 BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION trigger2_fn();"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'On main');"#,
                r#"SELECT * FROM dolt_checkout('other');"#,
                r#"CREATE FUNCTION trigger3_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.a := (NEW.a * 2) + 100; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger3 BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION trigger3_fn();"#,
                r#"CREATE FUNCTION trigger4_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.a := (NEW.a * 2) + 1000; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger4 BEFORE INSERT ON x FOR EACH ROW EXECUTE FUNCTION trigger4_fn();"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'On other');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT statement FROM dolt_patch('main', 'other') WHERE statement LIKE '%TRIGGER%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE TRIGGER trigger3 BEFORE INSERT ON public.x FOR EACH ROW EXECUTE FUNCTION public.trigger3_fn();")],
                            &[T("CREATE TRIGGER trigger4 BEFORE INSERT ON public.x FOR EACH ROW EXECUTE FUNCTION public.trigger4_fn();")],
                            &[T("DROP TRIGGER trigger2 ON public.x;")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_patch, whose statements drop trigger2 and create trigger3 and trigger4 to go from main to other")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '--no-commit');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Merged other table');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tgname FROM pg_trigger WHERE tgrelid = 'x'::regclass ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                            &[T("trigger1")],
                            &[T("trigger2")],
                            &[T("trigger3")],
                            &[T("trigger4")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust keeps views and triggers as schema objects of their own (STORAGE_DESIGN IV.3a), so pg_trigger and pg_views list them, where Dolt keeps them as rows of dolt_schemas")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"triggers: import with triggers"#,
            intent: r#"Loading a CSV file fires the table's insert triggers on each row it loads, and information_schema.triggers lists the trigger."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk BIGINT PRIMARY KEY, v1 BIGINT);"#,
                r#"CREATE FUNCTION trigger1_fn() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.v1 := NEW.v1 + 1; RETURN NEW; END$$;"#,
                r#"CREATE TRIGGER trigger1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger1_fn();"#,
                r#"INSERT INTO test VALUES (1, 1);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COPY test FROM STDIN WITH (FORMAT csv, HEADER);"#,
                    expected: Expected::Tag("COPY 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file")),
                    copy_from_stdin_file: "bats-triggers-import.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT trigger_name, event_manipulation, event_object_table, action_order, action_condition, action_statement, action_orientation, action_timing FROM information_schema.triggers;"#,
                    expected: Expected::Rows {
                        columns: &[Column("trigger_name", NAME), Column("event_manipulation", VARCHAR), Column("event_object_table", NAME), Column("action_order", INT4), Column("action_condition", VARCHAR), Column("action_statement", VARCHAR), Column("action_orientation", VARCHAR), Column("action_timing", VARCHAR)],
                        rows: &[
                            &[T("trigger1"), T("INSERT"), T("test"), T("1"), Null, T("EXECUTE FUNCTION trigger1_fn()"), T("ROW"), T("BEFORE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("MySQL's TRIGGERS table has the definer and character set columns and shows the trigger's statement, and Postgres' shows the function the trigger executes and its orientation")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
