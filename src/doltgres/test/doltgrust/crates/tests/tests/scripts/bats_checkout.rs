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

//! Dolt's integration-tests/bats/checkout.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_checkout() {
    run_scripts(&[
        ScriptTest {
            name: "checkout: dolt checkout table to restore working tree tables with add and drop foreign key",
            intent: "dolt_checkout of a table takes back a foreign key added to it since HEAD, and restores one dropped since.",
            set_up_script: &[
                "CREATE TABLE t (c1 int PRIMARY KEY, c2 int, CHECK (c2 > 0));",
                "CREATE TABLE z (c1 int PRIMARY KEY, c2 int);",
                "SELECT dolt_commit('-Am', 'create tables t and z');",
                "ALTER TABLE z ADD CONSTRAINT foreign_key1 FOREIGN KEY (c1) REFERENCES t (c1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.z"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them (Appendix C \"what is versioned\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'z'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("foreign_key1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('z');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'z'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE z ADD CONSTRAINT foreign_key1 FOREIGN KEY (c1) REFERENCES t (c1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add fkey');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE z DROP CONSTRAINT foreign_key1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.z"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them (Appendix C \"what is versioned\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'z'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('z');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'z'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("foreign_key1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: -B flag will forcefully reset an existing branch",
            intent: "dolt_checkout -B moves an existing branch to a start point, dropping its own commits.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (89012);",
                "SELECT dolt_commit('-Am', 'first change.');",
                "INSERT INTO test (id) VALUES (76543);",
                "SELECT dolt_commit('-Am', 'second change.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'testbr', 'main~1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'testbr'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("89012")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id) VALUES (19283);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'change to testbr.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'testbr', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'testbr'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("76543")],
                            &[T("89012")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: -B will create a branch that does not exist",
            intent: "dolt_checkout -B makes a branch that doesn't exist.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (89012);",
                "SELECT dolt_commit('-Am', 'first change.');",
                "INSERT INTO test (id) VALUES (76543);",
                "SELECT dolt_commit('-Am', 'second change.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-B', 'testbr', 'main~1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'testbr'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("89012")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: attempting to checkout a detached head shows a suggestion instead",
            intent: "dolt_checkout of a commit's hash fails, suggesting a new branch at it.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create test table.');",
                "SELECT set_config('test.sha', dolt_hashof('HEAD'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout(current_setting('test.sha'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt does not support a detached head state. To create a branch at this commit instead, run:", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: commit --amend only changes commit message",
            intent: "dolt_commit --amend with only a message replaces the last commit's message and keeps its rows.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (8);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'modified_commit_message');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("modified_commit_message")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message <> 'Initialize data repository';",
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
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
        ScriptTest {
            name: "checkout: commit --amend adds new changes to existing commit",
            intent: "dolt_commit --amend folds newly staged changes into the last commit with the new message.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
                "INSERT INTO test (id) VALUES (8);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
                "INSERT INTO test (id) VALUES (9);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'modified_commit_message');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("modified_commit_message")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message <> 'Initialize data repository';",
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
                    query: "SELECT * FROM test ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: table and branch name conflict with -- separator",
            intent: "With --, dolt_checkout of a name that is both a table and a branch restores the table and stays on the branch.",
            set_up_script: &[
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "INSERT INTO feature VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add feature table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO feature VALUES (2, 200);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add row to feature table');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE feature SET value = 101 WHERE id = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM feature;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: explicit branch checkout with -- separator",
            intent: "A name followed by -- is checked out as a branch even when a table has that name.",
            set_up_script: &[
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "INSERT INTO feature VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add feature table');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE feature SET value = 200 WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update feature value on feature branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM feature;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: error with multiple refs using --",
            intent: "dolt_checkout refuses more than one reference before --.",
            set_up_script: &[
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add feature table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1', 'branch2', '--', 'feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only one reference expected, 2 given", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: checkout multiple tables using --",
            intent: "dolt_checkout of a branch's tables after -- gives them that branch's rows and stays on the session's branch.",
            set_up_script: &[
                "CREATE TABLE table1 (id int PRIMARY KEY, value int);",
                "CREATE TABLE table2 (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO table1 VALUES (1, 100);",
                "INSERT INTO table2 VALUES (1, 'original');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add initial tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE table1 SET value = 200 WHERE id = 1;",
                "UPDATE table2 SET name = 'modified' WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update tables on feature branch');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE table1 SET value = 150 WHERE id = 1;",
                "UPDATE table2 SET name = 'changed' WHERE id = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--', 'table1', 'table2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: warning on missing table checkout",
            intent: "dolt_checkout of tables fails, naming each missing one, and a failed checkout leaves the tables as they were.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, value int);",
                "INSERT INTO test VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add test table');",
                "UPDATE test SET value = 200 WHERE id = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('missing_table');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing_table' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'test', 'missing_table');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing_table' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a statement that fails changes nothing in Postgres, so dolt_checkout restores none of the tables it names when one is missing, where Dolt restores the others (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'missing1', 'missing2');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'missing1' did not match any table(s) known to dolt
error: tablespec 'missing2' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --overwrite-ignore and --no-overwrite-ignore are mutually exclusive",
            intent: "dolt_checkout refuses --overwrite-ignore with --no-overwrite-ignore.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--overwrite-ignore', '--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --overwrite-ignore and --no-overwrite-ignore are mutually exclusive", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: dolt checkout . preserves untracked tables",
            intent: "dolt_checkout('.') gives tracked tables HEAD's rows and leaves a table never staged alone.",
            set_up_script: &[
                "CREATE TABLE normal_tbl (pk int PRIMARY KEY);",
                "SELECT dolt_add('normal_tbl');",
                "SELECT dolt_commit('-m', 'add normal_tbl');",
                "CREATE TABLE untracked_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO untracked_tbl VALUES (1, 42);",
                "INSERT INTO normal_tbl VALUES (99);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('.');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM normal_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, val FROM untracked_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: dolt checkout table from another branch",
            intent: "dolt_checkout of a table from a branch gives it that branch's rows and leaves the other tables' changes.",
            set_up_script: &[
                "CREATE TABLE t (c1 int PRIMARY KEY, c2 int, CHECK (c2 > 0));",
                "CREATE TABLE z (c1 int PRIMARY KEY, c2 int);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO z VALUES (2, 2);",
                "SELECT dolt_commit('-Am', 'new values in t');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t VALUES (3, 3);",
                "INSERT INTO z VALUES (4, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1', '--', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.z"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads active_branch and dolt_status")),
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
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM z;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "checkout: commit --amend on merge commits does not modify metadata of merged parents",
            intent: "Amending a merge commit keeps both of its parents.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, id2 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original table');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test (id, id2) VALUES (0, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'conflicting commit message');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test (id, id2) VALUES (0, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts_resolve('--theirs', '.');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'final merge') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'new merge') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.message FROM dolt_commit_ancestors AS p JOIN dolt_log AS c ON c.commit_hash = p.parent_hash WHERE p.commit_hash = dolt_hashof('HEAD') ORDER BY p.parent_index;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("original commit message")],
                            &[T("conflicting commit message")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log --parents is a CLI command, so the port reads the amended merge's parents from dolt_commit_ancestors")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: dolt_commit --amend on merge commits does not modify metadata of merged parents",
            intent: "dolt_commit --amend on a merge commit keeps both of its parents.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, id2 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original table');",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test (id, id2) VALUES (0, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'conflicting commit message');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test (id, id2) VALUES (0, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'original commit message');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts_resolve('--theirs', '.');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'final merge') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'new merge') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.message FROM dolt_commit_ancestors AS p JOIN dolt_log AS c ON c.commit_hash = p.parent_hash WHERE p.commit_hash = dolt_hashof('HEAD') ORDER BY p.parent_index;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("original commit message")],
                            &[T("conflicting commit message")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log --parents is a CLI command, so the port reads the amended merge's parents from dolt_commit_ancestors")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: checkout specific table from branch",
            intent: "dolt_checkout of one table from a branch leaves the other tables and the current branch alone.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(50));",
                "CREATE TABLE products (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO users VALUES (1, 'Alice');",
                "INSERT INTO products VALUES (1, 'Widget');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add initial tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE users SET name = 'Bob' WHERE id = 1;",
                "UPDATE products SET name = 'Gadget' WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update data on feature branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--', 'users');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM products;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Widget")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: remote tracking branch shorthand",
            intent: "dolt_checkout of a name only a remote-tracking branch has creates the local branch from it.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote-repo}');",
                "SELECT * FROM dolt_push('-u', 'origin', 'main');",
                "CREATE TABLE test (id int PRIMARY KEY, val int);",
                "INSERT INTO test VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE test SET val = 200 WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update on feature branch');",
                "SELECT * FROM dolt_push('origin', 'feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remote_branches WHERE name = 'remotes/origin/feature';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("remotes/origin/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch -a is a CLI command, so the port reads dolt_remote_branches")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: error on ambiguous name matching tracking branch and table",
            intent: "dolt_checkout refuses a name that is both a table and a remote-tracking branch, and -- picks either.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote-repo}');",
                "SELECT * FROM dolt_push('-u', 'origin', 'main');",
                "CREATE TABLE test (id int PRIMARY KEY, val int);",
                "INSERT INTO test VALUES (1, 50);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE test SET val = 200 WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update on feature branch');",
                "SELECT * FROM dolt_push('origin', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "INSERT INTO feature VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Create table with same name as tracking branch');",
                "SELECT dolt_branch('-D', 'feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "'feature' could be both a local table and a tracking branch.\nPlease use -- to disambiguate.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM feature;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: default to local branch checkout after disambiguation",
            intent: "Once -- created the local branch, dolt_checkout of the name switches to it.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote-repo}');",
                "SELECT * FROM dolt_push('-u', 'origin', 'main');",
                "CREATE TABLE test (id int PRIMARY KEY, val int);",
                "INSERT INTO test VALUES (1, 50);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "UPDATE test SET val = 200 WHERE id = 1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Update on feature branch');",
                "SELECT * FROM dolt_push('origin', 'feature');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE feature (id int PRIMARY KEY, value int);",
                "INSERT INTO feature VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Create table with same name as tracking branch');",
                "SELECT dolt_branch('-D', 'feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature', '--');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: more than one remote share same branch name",
            intent: "dolt_checkout refuses a name two remotes' tracking branches share, and --track picks one.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote1}');",
                "SELECT dolt_remote('add', 'origin2', 'file://{NEWDIR:remote2}');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "CREATE TABLE test (id int PRIMARY KEY, value int);",
                "INSERT INTO test VALUES (1, 100);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add feature table');",
                "SELECT * FROM dolt_push('origin', 'feature');",
                "SELECT * FROM dolt_push('origin2', 'feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_remote_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("remotes/origin/feature")],
                            &[T("remotes/origin2/feature")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch -a is a CLI command, so the port reads dolt_remote_branches")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'feature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "hint: If you meant to check out a remote tracking branch, on, e.g., 'origin',\nhint: you can do so by fully qualifying the name with the --track option:\nhint:\nhint:        dolt checkout --track origin/<name>\n'feature' matched multiple (2) remote tracking branches", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--track', 'origin/feature');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'\nbranch 'feature' set up to track 'origin/feature'.")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --no-overwrite-ignore aborts when ignored table would be overwritten",
            intent: "dolt_checkout --no-overwrite-ignore refuses a switch that would overwrite an ignored table.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:\n\tignored_tbl\nPlease move or remove them before you switch branches.\nUse --overwrite-ignore to force.\n", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --overwrite-ignore allows checkout when ignored table would be overwritten",
            intent: "dolt_checkout --overwrite-ignore switches even when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--overwrite-ignore', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: default behavior overwrites ignored tables (no flag)",
            intent: "dolt_checkout without a flag switches even when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table and ignore pattern on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --no-overwrite-ignore allows checkout when ignored table is unchanged",
            intent: "dolt_checkout --no-overwrite-ignore switches when the ignored table is the same on both branches.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE normal_tbl (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'add normal table on other branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --no-overwrite-ignore does not warn when ignored table only exists on target branch",
            intent: "dolt_checkout --no-overwrite-ignore switches when only the target branch has the ignored table.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --force combined with --no-overwrite-ignore still blocks ignored table overwrite",
            intent: "dolt_checkout --force --no-overwrite-ignore still refuses to overwrite an ignored table.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "CREATE TABLE normal_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO normal_tbl VALUES (1, 1);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add tables on main', '--force');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "INSERT INTO normal_tbl VALUES (2, 2);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify tables on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO normal_tbl VALUES (3, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--force', '--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:\n\tignored_tbl\nPlease move or remove them before you switch branches.\nUse --overwrite-ignore to force.\n", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --no-overwrite-ignore with multiple ignored tables only reports overwritten ones",
            intent: "dolt_checkout --no-overwrite-ignore names only the ignored tables the switch would change.",
            set_up_script: &[
                "CREATE TABLE ign_changed (pk int PRIMARY KEY, val int);",
                "INSERT INTO ign_changed VALUES (1, 100);",
                "CREATE TABLE ign_same (pk int PRIMARY KEY, val int);",
                "INSERT INTO ign_same VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ign_%', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add two ignored tables on main', '--force');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ign_changed VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify one ignored table on other branch', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:\n\tign_changed\nPlease move or remove them before you switch branches.\nUse --overwrite-ignore to force.\n", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: -b with --no-overwrite-ignore does not block new branch from HEAD",
            intent: "dolt_checkout -b --no-overwrite-ignore from HEAD never refuses.",
            set_up_script: &[
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--no-overwrite-ignore');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'newbranch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("newbranch")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch's current-branch mark is the CLI's, so the port reads active_branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: --no-overwrite-ignore does not warn when ignored table only exists locally",
            intent: "dolt_checkout --no-overwrite-ignore switches to a branch that never had the ignored table.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: dolt checkout HEAD <table> leaves ignored tables unstaged",
            intent: "dolt_checkout of a table from HEAD doesn't stage an ignored table.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('private_data', true);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'add ignore rule for private_data');",
                "CREATE TABLE mytable (pk int PRIMARY KEY, val int);",
                "INSERT INTO mytable VALUES (1, 10);",
                "SELECT dolt_add('mytable');",
                "SELECT dolt_commit('-m', 'add mytable');",
                "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                "INSERT INTO private_data VALUES (1, 'secret');",
                "INSERT INTO mytable VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('HEAD', '--', 'mytable');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_status WHERE table_name = 'public.private_data';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checkout: dolt checkout . preserves ignored tables",
            intent: "dolt_checkout('.') gives tracked tables HEAD's rows and leaves an ignored table alone.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', true);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'add ignore rule');",
                "CREATE TABLE normal_tbl (pk int PRIMARY KEY);",
                "SELECT dolt_add('normal_tbl');",
                "SELECT dolt_commit('-m', 'add normal table');",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO normal_tbl VALUES (99);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('.');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM normal_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, val FROM ignored_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
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
