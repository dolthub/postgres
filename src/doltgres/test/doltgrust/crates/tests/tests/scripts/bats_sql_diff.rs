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

//! Dolt's integration-tests/bats/sql-diff.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_diff() {
    run_scripts(&[
        ScriptTest {
            name: "sql-diff: output reconciles INSERT query",
            intent: "Running dolt_patch's statements on one branch brings over an inserted row from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5), (1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (2, 11, 0, 0, 0, 0);",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles UPDATE query",
            intent: "Running dolt_patch's statements on one branch brings over an updated row from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5), (1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "UPDATE test SET c1 = 11, c5 = 6 WHERE pk = 0;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles DELETE query",
            intent: "Running dolt_patch's statements on one branch brings over a deleted row from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5), (1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "DELETE FROM test WHERE pk = 0;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles change to PRIMARY KEY field in row",
            intent: "Running dolt_patch's statements on one branch brings over a row whose key changed from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5), (1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "UPDATE test SET pk = 2 WHERE pk = 1;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles column rename",
            intent: "Running dolt_patch's statements on one branch brings over a renamed column from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "ALTER TABLE test RENAME COLUMN c1 TO c0;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles DROP column query",
            intent: "Running dolt_patch's statements on one branch brings over a dropped column from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "ALTER TABLE test DROP COLUMN c1;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: output reconciles ADD column query",
            intent: "Running dolt_patch's statements on one branch brings over an added column from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "ALTER TABLE test ADD c0 bigint;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: reconciles CREATE TABLE",
            intent: "Running dolt_patch's statements on one branch brings over a new table from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: includes row INSERTSs to new tables after CREATE TABLE",
            intent: "Running dolt_patch's statements on one branch brings over a new table and its rows from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "INSERT INTO test VALUES (2, 2, 2, 2, 2, 2);",
                "INSERT INTO test VALUES (3, 3, 3, 3, 3, 3);",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: reconciles DROP TABLE",
            intent: "Running dolt_patch's statements on one branch brings over a dropped table from the other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "DROP TABLE test;",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: reconciles RENAME TABLE",
            intent: "dolt_patch renames a renamed table and brings over its new rows.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "ALTER TABLE test RENAME TO newname;",
                "INSERT INTO newname VALUES (2, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('firstbranch', 'newbranch') WHERE diff_type = 'schema';",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.test RENAME TO newname;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: reconciles RENAME TABLE with schema changes",
            intent: "dolt_patch renames a table and its changed columns, and brings over its rows, which the CLI leaves out with a warning.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'first');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "ALTER TABLE test RENAME COLUMN c2 TO col2;",
                "ALTER TABLE test ADD COLUMN c6 int;",
                "ALTER TABLE test RENAME TO newname;",
                "ALTER TABLE newname DROP COLUMN c3;",
                "INSERT INTO newname VALUES (2, 1, 1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('firstbranch', 'newbranch') WHERE diff_type = 'schema' ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.test RENAME TO newname;")],
                            &[T("ALTER TABLE public.newname RENAME COLUMN c2 TO col2;")],
                            &[T("ALTER TABLE public.newname DROP COLUMN c3;")],
                            &[T("ALTER TABLE public.newname ADD COLUMN c6 integer;")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: reconciles CREATE/ALTER/DROP VIEW",
            intent: "dolt_patch writes a new view's creation after its table's.",
            set_up_script: &[
                "CREATE TABLE test (pk int NOT NULL PRIMARY KEY);",
                "CREATE VIEW double_view AS SELECT pk * 2 FROM test;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("CREATE TABLE public.test (\n    pk integer NOT NULL\n);")],
                            &[T("public.test"), T("ALTER TABLE public.test OWNER TO postgres;")],
                            &[T("public.test"), T("ALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                            &[T("view public.double_view"), T("CREATE VIEW public.double_view AS\n SELECT (pk * 2) AS \"?column?\"\n   FROM public.test;")],
                            &[T("view public.double_view"), T("ALTER VIEW public.double_view OWNER TO postgres;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: diff sql recreates tables with all types",
            intent: "dolt_patch's creation of a table with every common type makes the same table.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "SELECT * FROM dolt_checkout('-b', 'newbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, \"int\" bigint, string text, \"boolean\" boolean, \"float\" double precision, uint bigint, uuid char(36), PRIMARY KEY (pk));",
                "SELECT dolt_commit('-Am', 'second');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("BIGINT UNSIGNED, LONGTEXT, and ascii CHAR(36) are MySQL's; bigint, text, and char(36) stand for them")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports all types",
            intent: "dolt_patch writes deletes, inserts, updates, and new tables of every common type.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk bigint NOT NULL, \"int\" bigint, string text, \"boolean\" boolean, \"float\" double precision, uint bigint, uuid char(36), PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 'asdf', TRUE, 0.0, 0, '00000000-0000-0000-0000-000000000000'), (1, -1, 'qwerty', FALSE, -1.0, 1, '00000000-0000-0000-0000-000000000001'), (2, 1, '', TRUE, 0.0, 0, '123e4567-e89b-12d3-a456-426655440000');",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'applied query');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, \"int\", string, \"boolean\", \"float\", uint, uuid) VALUES (3, 3, 'three', TRUE, 3.3, 3, '00000000-0000-0000-0000-000000000003');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'applied query');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET \"int\" = -1, \"boolean\" = FALSE, uint = 2 WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'applied query');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE newtable (pk bigint NOT NULL, \"int\" bigint, string text, \"boolean\" boolean, \"float\" double precision, uint bigint, uuid text, PRIMARY KEY (pk));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'applied query');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (delete)",
            intent: "dolt_patch reconciles a delete on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "DELETE FROM test WHERE pk1 = 1 AND pk2 = 0;",
                "SELECT dolt_commit('-Am', 'applied delete query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (add)",
            intent: "dolt_patch reconciles a add on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "INSERT INTO test (pk1, pk2, c1, c2, c3, c4, c5) VALUES (2, 2, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'applied add query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (update)",
            intent: "dolt_patch reconciles a update on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "UPDATE test SET c1 = 9, c3 = 9, c5 = 9 WHERE pk1 = 1 AND pk2 = 1;",
                "SELECT dolt_commit('-Am', 'applied update query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (single_pk_update)",
            intent: "dolt_patch reconciles a single pk update on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "UPDATE test SET pk1 = 3 WHERE pk1 = 0 AND pk2 = 1;",
                "SELECT dolt_commit('-Am', 'applied single_pk_update query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (all_pk_update)",
            intent: "dolt_patch reconciles a all pk update on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "UPDATE test SET pk1 = 3, pk2 = 3 WHERE pk1 = 1 AND pk2 = 1;",
                "SELECT dolt_commit('-Am', 'applied all_pk_update query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: supports multiple primary keys (create_table)",
            intent: "dolt_patch reconciles a create table on a table with a two-column key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'firstbranch');",
                "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5), (0, 1, 1, 2, 3, 4, 5), (1, 0, 1, 2, 3, 4, 5), (1, 1, 1, 2, 3, 4, 5);",
                "SELECT dolt_commit('-Am', 'create/init table test');",
                "SELECT dolt_branch('newbranch');",
                "SELECT * FROM dolt_checkout('newbranch');",
                "CREATE TABLE newtable (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                "SELECT dolt_commit('-Am', 'applied create_table query');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM dolt_patch('firstbranch', 'newbranch')) AS differs;",
                    expected: Expected::Rows {
                        columns: &[Column("differs", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('firstbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('firstbranch', 'newbranch') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Reconciled with newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('firstbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: escapes values for MySQL string literals",
            intent: "dolt_patch writes text values with backslashes, quotes, and control characters so they read back the same.",
            set_up_script: &[
                "CREATE TABLE test (pk int NOT NULL, c1 text, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'created table');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test (pk, c1) VALUES (0, E'\\\\');",
                "INSERT INTO test (pk, c1) VALUES (1, E'this string ends in backslash\\\\');",
                "INSERT INTO test (pk, c1) VALUES (2, 'this string has \"double quotes\" in it');",
                "INSERT INTO test (pk, c1) VALUES (3, 'it''s a contraction y''all');",
                "INSERT INTO test (pk, c1) VALUES (4, E'backspace \\b');",
                "INSERT INTO test (pk, c1) VALUES (5, E'newline \\n');",
                "INSERT INTO test (pk, c1) VALUES (6, E'carriage return \\r');",
                "INSERT INTO test (pk, c1) VALUES (7, E'tab \\t');",
                "INSERT INTO test (pk, c1) VALUES (8, E'ASCII 26 (Control+Z) \\x1a');",
                "INSERT INTO test (pk, c1) VALUES (9, 'percent \\%');",
                "INSERT INTO test (pk, c1) VALUES (10, 'underscore \\_');",
                "INSERT INTO test (pk, c1) VALUES (11, '\"\"');",
                "INSERT INTO test (pk, c1) VALUES (12, '\"');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tricky rows');",
                "SELECT * FROM dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('other', 'main') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_patch('main', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
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
            name: "sql-diff: handles NULL cells",
            intent: "dolt_patch writes NULL cells as NULL, for text and timestamp columns.",
            set_up_script: &[
                "CREATE TABLE test (pk int NOT NULL, c1 text, PRIMARY KEY (pk));",
                "INSERT INTO test (pk, c1) VALUES (0, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') WHERE diff_type = 'data';",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.test (pk, c1) VALUES (0, NULL);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, c1 timestamp, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1) VALUES (0, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') WHERE diff_type = 'data';",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.test (pk, c1) VALUES (0, NULL);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: views, triggers, tables",
            intent: "dolt_patch writes table rows first, then functions, triggers, and views, each in pg_dump's text, and replaces changed ones.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (1, 2, 3);",
                "INSERT INTO test VALUES (4, 5, 6);",
                "SELECT dolt_commit('-am', 'Table with rows');",
                "INSERT INTO test VALUES (7, 8, 9);",
                "DELETE FROM test WHERE pk = 4;",
                "SELECT dolt_commit('-Am', 'Table data diff');",
                "CREATE VIEW v1 AS SELECT 'hello' FROM test;",
                "SELECT dolt_commit('-Am', 'View');",
                "CREATE FUNCTION tr1_fn() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN NEW.c1 := NEW.c1 + 1; RETURN NEW; END $f$;",
                "CREATE TRIGGER tr1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION tr1_fn();",
                "SELECT dolt_commit('-Am', 'Trigger');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~3', 'HEAD~2') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.test WHERE pk = 4;")],
                            &[T("INSERT INTO public.test (pk, c1, c2) VALUES (7, 8, 9);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~2', 'HEAD~') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE VIEW public.v1 AS\n SELECT 'hello'::text AS \"?column?\"\n   FROM public.test;")],
                            &[T("ALTER VIEW public.v1 OWNER TO postgres;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~', 'HEAD') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE FUNCTION public.tr1_fn() RETURNS trigger\n    LANGUAGE plpgsql\n    AS $$ BEGIN NEW.c1 := NEW.c1 + 1; RETURN NEW; END $$;")],
                            &[T("ALTER FUNCTION public.tr1_fn() OWNER TO postgres;")],
                            &[T("CREATE TRIGGER tr1 BEFORE INSERT ON public.test FOR EACH ROW EXECUTE FUNCTION public.tr1_fn();")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~2', 'HEAD') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE FUNCTION public.tr1_fn() RETURNS trigger\n    LANGUAGE plpgsql\n    AS $$ BEGIN NEW.c1 := NEW.c1 + 1; RETURN NEW; END $$;")],
                            &[T("ALTER FUNCTION public.tr1_fn() OWNER TO postgres;")],
                            &[T("CREATE TRIGGER tr1 BEFORE INSERT ON public.test FOR EACH ROW EXECUTE FUNCTION public.tr1_fn();")],
                            &[T("CREATE VIEW public.v1 AS\n SELECT 'hello'::text AS \"?column?\"\n   FROM public.test;")],
                            &[T("ALTER VIEW public.v1 OWNER TO postgres;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~3', 'HEAD~') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.test WHERE pk = 4;")],
                            &[T("INSERT INTO public.test (pk, c1, c2) VALUES (7, 8, 9);")],
                            &[T("CREATE VIEW public.v1 AS\n SELECT 'hello'::text AS \"?column?\"\n   FROM public.test;")],
                            &[T("ALTER VIEW public.v1 OWNER TO postgres;")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~3', 'HEAD') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.test WHERE pk = 4;")],
                            &[T("INSERT INTO public.test (pk, c1, c2) VALUES (7, 8, 9);")],
                            &[T("CREATE FUNCTION public.tr1_fn() RETURNS trigger\n    LANGUAGE plpgsql\n    AS $$ BEGIN NEW.c1 := NEW.c1 + 1; RETURN NEW; END $$;")],
                            &[T("ALTER FUNCTION public.tr1_fn() OWNER TO postgres;")],
                            &[T("CREATE TRIGGER tr1 BEFORE INSERT ON public.test FOR EACH ROW EXECUTE FUNCTION public.tr1_fn();")],
                            &[T("CREATE VIEW public.v1 AS\n SELECT 'hello'::text AS \"?column?\"\n   FROM public.test;")],
                            &[T("ALTER VIEW public.v1 OWNER TO postgres;")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION tr1_fn() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN NEW.c1 := NEW.c1 + 100; RETURN NEW; END $f$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW v1 AS SELECT 'goodbye' FROM test;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'new view and trigger defs');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~', 'HEAD') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION public.tr1_fn() RETURNS trigger\n    LANGUAGE plpgsql\n    AS $$ BEGIN NEW.c1 := NEW.c1 + 100; RETURN NEW; END $$;")],
                            &[T("CREATE OR REPLACE VIEW public.v1 AS\n SELECT 'goodbye'::text AS \"?column?\"\n   FROM public.test;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: skinny flag comparison between CLI and SQL table function",
            intent: "dolt_diff refuses an unknown option and an option given twice.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 1, 2, 3, 4, 5), (1, 1, 2, 3, 4, 5);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'Added initial data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('-err', 'HEAD~1', 'HEAD', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unknown option `err", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('-sk', '--skinny', 'HEAD~1', 'HEAD', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "multiple values provided for `skinny", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: ignored tables in working set are skipped",
            intent: "The dolt_diff table leaves out a table dolt_ignore names and shows the change to dolt_ignore itself.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignore_*', true);",
                "CREATE TABLE ignore_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create ignored table');",
                "CREATE TABLE test_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, message FROM dolt_diff;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("public.test_table"), Null],
                            &[T("dolt_ignore"), T("create ignored table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: stored generated columns omitted from INSERT and UPDATE",
            intent: "dolt_patch leaves stored generated columns out of its INSERT and UPDATE statements, so running them computes the columns.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, a int, g int GENERATED ALWAYS AS (a + 1) STORED);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'create table with stored generated column');",
                "INSERT INTO t (id, a) VALUES (1, 10);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'seed row');",
                "UPDATE t SET a = 99 WHERE id = 1;",
                "INSERT INTO t (id, a) VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("UPDATE public.t SET a = 99 WHERE id = 1;")],
                            &[T("INSERT INTO public.t (id, a) VALUES (2, 20);")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE patch AS SELECT statement_order, statement FROM dolt_patch('HEAD', 'WORKING');",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM patch ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, g FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("99"), T("100")],
                            &[T("2"), T("20"), T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-diff: patch applies to the from state",
            intent: "Running dolt_patch's statements on a branch at the from commit gives the to commit's root: a changed function a default calls, a view over a column whose type changed, and a dropped column.",
            set_up_script: &[
                "CREATE FUNCTION f() RETURNS int LANGUAGE sql AS $f$ SELECT 1 $f$;",
                "CREATE TABLE t (pk int PRIMARY KEY, a int DEFAULT f(), b int, c int);",
                "CREATE VIEW v AS SELECT pk, a, b FROM t;",
                "INSERT INTO t (pk, b, c) VALUES (1, 10, 100), (2, 20, 200);",
                "SELECT dolt_commit('-Am', 'from');",
                "SELECT dolt_branch('copy');",
                "CREATE OR REPLACE FUNCTION f() RETURNS int LANGUAGE sql AS $f$ SELECT 2 $f$;",
                "ALTER TABLE t ADD COLUMN d int DEFAULT f();",
                "DROP VIEW v;",
                "ALTER TABLE t ALTER COLUMN b TYPE bigint;",
                "CREATE VIEW v AS SELECT pk, a, b FROM t;",
                "ALTER TABLE t DROP COLUMN c;",
                "INSERT INTO t (pk, b) VALUES (3, 30);",
                "SELECT dolt_commit('-am', 'to');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('copy');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE s text; BEGIN FOR s IN SELECT statement FROM dolt_patch('copy', 'main') ORDER BY statement_order LOOP EXECUTE s; END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db('WORKING') = dolt_hashof_db('main') AS same;",
                    expected: Expected::Rows {
                        columns: &[Column("same", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: after the patch, no statement takes the branch to the to commit.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_patch('WORKING', 'main');",
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
