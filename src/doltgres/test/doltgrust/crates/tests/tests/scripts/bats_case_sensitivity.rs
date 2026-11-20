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

//! Dolt's integration-tests/bats/case-sensitivity.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_case_sensitivity() {
    run_scripts(&[
        ScriptTest {
            name: r#"case-sensitivity: dolt system tables & db names"#,
            intent: r#"Dolt's system tables and information_schema answer under names written in any case."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'random commit');"#,
                r#"INSERT INTO test VALUES (3, 'ccc', 'CCC');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT to_bbb FROM dOlT_dIfF_tEsT;"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_bbb", VARCHAR)],
                        rows: &[
                            &[T("aaa")],
                            &[T("bbb")],
                            &[T("ccc")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT DISTINCT table_schema FROM InFoRmAtIoN_sChEmA.tAbLeS WHERE table_schema = 'information_schema';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_schema", NAME)],
                        rows: &[
                            &[T("information_schema")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Some commit');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dOlT_cOmMiT_dIfF_tEsT WHERE dolt_to_commit = 'WORKING' AND dolt_from_commit = 'WORKING';"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dOlT_hIsToRy_tEsT;"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dOlT_cOnFlIcTs_tEsT;"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dOlT_cOnStRaInT_vIoLaTiOnS_tEsT;"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: capital letter col names. sql select with a where clause"#,
            intent: r#"A column declared with capitals answers to its name in a WHERE clause."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE Aaa = 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4), Column("bbb", VARCHAR), Column("ccc", VARCHAR)],
                        rows: &[
                            &[T("2"), T("bbb"), T("BBB")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, bbb, and ccc, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: capital letter col names. dolt schema show"#,
            intent: r#"The table's schema lists the columns declared with capitals."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT attname FROM pg_attribute WHERE attrelid = 'test'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("aaa")],
                            &[T("bbb")],
                            &[T("ccc")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show is a CLI command, so the port lists pg_attribute, and Postgres folds unquoted names to lower case")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: capital letter col names. sql select"#,
            intent: r#"A query of one capitalized column returns that column alone."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT Bbb FROM test WHERE Aaa=2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("bbb", VARCHAR)],
                        rows: &[
                            &[T("bbb")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as bbb, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: capital letter col names. dolt table copy"#,
            intent: r#"A copy of a table with capitalized columns keeps its columns and rows."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 AS TABLE test;"#,
                    expected: Expected::Tag("SELECT 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table cp is a CLI command, so the port copies the table with CREATE TABLE AS")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4), Column("bbb", VARCHAR), Column("ccc", VARCHAR)],
                        rows: &[
                            &[T("1"), T("aaa"), T("AAA")],
                            &[T("2"), T("bbb"), T("BBB")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, bbb, and ccc, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: capital letter column names. select with an as"#,
            intent: r#"A capitalized column can be selected under an alias."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT Aaa AS AAA FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: select table case insensitive, table and column"#,
            intent: r#"Tables and columns are found whatever case a query writes them in."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT aaa AS AAA FROM TEST ORDER BY 1 LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, where MySQL keeps the case it was written in")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT Test.aaa AS AAA FROM TEST ORDER BY 1 LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: select table alias case insensitive"#,
            intent: r#"A table alias is found whatever case a query writes it in."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT t.aaA AS AaA FROM TEST T ORDER BY 1 LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: select reserved word, preserve case"#,
            intent: r#"A reserved word works as a column alias."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 1 AS DaTe;"#,
                    expected: Expected::Rows {
                        columns: &[Column("date", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as date, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"case-sensitivity: select column with different case"#,
            intent: r#"A column is found when a query writes it in another case than its declaration."#,
            set_up_script: &[
                r#"CREATE TABLE test (Aaa INT NOT NULL, Bbb VARCHAR(20), Ccc VARCHAR(20), PRIMARY KEY (Aaa));"#,
                r#"INSERT INTO test VALUES (1, 'aaa', 'AAA'), (2, 'bbb', 'BBB');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT AaA FROM Test ORDER BY 1 LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("aaa", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres folds an unquoted name to lower case, so the column shows as aaa, where MySQL keeps the case it was written in")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
