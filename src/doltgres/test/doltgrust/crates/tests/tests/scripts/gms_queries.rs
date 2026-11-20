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

//! Dolt's go-mysql-server enginetest/queries/queries.go, ported at Dolt 614771d742: the AS OF tests Dolt runs through
//! `TestVersionedQueries` and `TestVersionedViews`, whose harness makes each snapshot a commit and a branch named for
//! its date.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_versioned_queries() {
    run_scripts(&[
        ScriptTest {
            name: "SELECT * FROM myhistorytable AS OF '2019-01-01' AS foo ORDER BY i",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable AS OF '2019-01-01' AS foo ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 1")],
                            &[T("2"), T("second row, 1")],
                            &[T("3"), T("third row, 1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myhistorytable AS OF '2019-01-02' foo ORDER BY i",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable AS OF '2019-01-02' foo ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 2")],
                            &[T("2"), T("second row, 2")],
                            &[T("3"), T("third row, 2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myhistorytable AS OF GREATEST('2019-01-02','2019-01-01','') foo ORDER BY i",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable AS OF GREATEST('2019-01-02', '2019-01-01', '') foo ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 2")],
                            &[T("2"), T("second row, 2")],
                            &[T("3"), T("third row, 2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myhistorytable ORDER BY i",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                            &[T("2"), T("second row, 3"), T("2")],
                            &[T("3"), T("third row, 3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SHOW TABLES AS OF '2019-01-02' LIKE 'myhistorytable'",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables AS OF '2019-01-02' WHERE table_schema = 'public' AND table_name LIKE 'myhistorytable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("myhistorytable")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SHOW TABLES FROM mydb AS OF '2019-01-02' LIKE 'myhistorytable'",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables AS OF '2019-01-02' WHERE table_schema = 'public' AND table_name LIKE 'myhistorytable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("myhistorytable")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SHOW CREATE TABLE myhistorytable as of '2019-01-02'",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns AS OF '2019-01-02' WHERE table_name = 'myhistorytable' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("i"), T("bigint"), T("NO")],
                            &[T("s"), T("text"), T("NO")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SHOW CREATE TABLE myhistorytable as of '2019-01-03'",
            intent: "A table read AS OF a branch named for a date reads the table as that branch has it.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns AS OF '2019-01-03' WHERE table_name = 'myhistorytable' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("i"), T("bigint"), T("NO")],
                            &[T("s"), T("text"), T("NO")],
                            &[T("c"), T("text"), T("NO")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_versioned_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "user var for AS OF expression",
            intent: "A session setting names the revision AS OF reads, and a table's columns as of a revision are those it had then.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "SELECT set_config('test.rev1', '2019-01-01', false);",
                "SELECT set_config('test.rev2', '2019-01-02', false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable AS OF current_setting('test.rev1') AS foo ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 1")],
                            &[T("2"), T("second row, 1")],
                            &[T("3"), T("third row, 1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM myhistorytable AS OF current_setting('test.rev2') AS foo ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 2")],
                            &[T("2"), T("second row, 2")],
                            &[T("3"), T("third row, 2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables AS OF current_setting('test.rev1') WHERE table_schema = 'public' AND table_name LIKE 'myhistorytable';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("myhistorytable")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns AS OF '2019-01-02' WHERE table_name = 'myhistorytable' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("i"), T("bigint"), T("NO")],
                            &[T("s"), T("text"), T("NO")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns AS OF '2019-01-03' WHERE table_name = 'myhistorytable' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("i"), T("bigint"), T("NO")],
                            &[T("s"), T("text"), T("NO")],
                            &[T("c"), T("text"), T("NO")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES, SHOW CREATE TABLE, and DESCRIBE are MySQL's, so the port reads information_schema AS OF the revision")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_versioned_view_tests() {
    run_scripts(&[
        ScriptTest {
            name: "SELECT * FROM myview1 ORDER BY i",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview1 ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                            &[T("2"), T("second row, 3"), T("2")],
                            &[T("3"), T("third row, 3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT t.* FROM myview1 AS t ORDER BY i",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t.* FROM myview1 AS t ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                            &[T("2"), T("second row, 3"), T("2")],
                            &[T("3"), T("third row, 3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT t.i FROM myview1 AS t ORDER BY i",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t.i FROM myview1 AS t ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8)],
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
            name: "SELECT * FROM myview1 AS OF '2019-01-01' ORDER BY i",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview1 AS OF '2019-01-01' ORDER BY i;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview2",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT i FROM myview2",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT i FROM myview2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8)],
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
            name: "SELECT myview2.i FROM myview2",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT myview2.i FROM myview2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8)],
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
            name: "SELECT myview2.* FROM myview2",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT myview2.* FROM myview2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT t.* FROM myview2 as t",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t.* FROM myview2 AS t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("first row, 3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT t.i FROM myview2 as t",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t.i FROM myview2 AS t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8)],
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
            name: "SELECT * FROM myview2 AS OF '2019-01-01'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview2 AS OF '2019-01-01';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview3 AS OF '2019-01-01'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview3 AS OF '2019-01-01' ORDER BY i;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview3 AS OF '2019-01-02'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview3 AS OF '2019-01-02' ORDER BY i;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview3 AS OF '2019-01-03'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview3 AS OF '2019-01-03' ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("i", TEXT)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("first row, 3")],
                            &[T("second row, 3")],
                            &[T("third row, 3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview4 AS OF '2019-01-01'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview4 AS OF '2019-01-01';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview4 AS OF '2019-01-02'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview4 AS OF '2019-01-02';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview4 AS OF '2019-01-03'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview4 AS OF '2019-01-03';",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("3"), T("third row, 3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview5 AS OF '2019-01-01'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview5 AS OF '2019-01-01';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column myhistorytable.c does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview5 AS OF '2019-01-02'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview5 AS OF '2019-01-02';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column myhistorytable.c does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Postgres expands a view's * when the view is made, so its current definition names column c, which the revision's myhistorytable lacks, where MySQL expands it again at each read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT * FROM myview5 AS OF '2019-01-03'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM myview5 AS OF '2019-01-03';",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("s", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("3"), T("third row, 3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select TABLE_CATALOG, TABLE_SCHEMA, TABLE_NAME, VIEW_DEFINITION, CHECK_OPTION, DEFINER, SECURITY_TYPE, CHARACTER_SET_CLIENT, COLLATION_CONNECTION from information_schema.views where table_schema = 'mydb'",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, view_definition FROM information_schema.views WHERE table_schema = 'public' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("view_definition", VARCHAR)],
                        rows: &[
                            &[T("myview1"), T(" SELECT i,
    s,
    c
   FROM myhistorytable;")],
                            &[T("myview2"), T(" SELECT i,
    s,
    c
   FROM myview1
  WHERE (i = 1);")],
                            &[T("myview3"), T(" SELECT (myview1.i)::text AS i
   FROM myview1
UNION
 SELECT myhistorytable.s AS i
   FROM myhistorytable;")],
                            &[T("myview4"), T(" SELECT i,
    s,
    c
   FROM myhistorytable
  WHERE (i IN ( SELECT DISTINCT (\"right\"(myhistorytable_1.s, 1))::bigint AS \"right\"
           FROM myhistorytable myhistorytable_1));")],
                            &[T("myview5"), T(" SELECT i,
    s,
    c
   FROM ( SELECT myhistorytable.i,
            myhistorytable.s,
            myhistorytable.c
           FROM myhistorytable
          WHERE (myhistorytable.i IN ( SELECT DISTINCT (\"right\"(myhistorytable.s, 1))::bigint AS \"right\"))) sq;")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::MySqlOnly("information_schema.views holds MySQL's definer, character set, and collation columns and the view's text as written, and Postgres' holds the view's definition as pg_get_viewdef writes it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select table_name from information_schema.tables where table_schema = 'mydb' and table_type = 'VIEW' order by 1",
            intent: "A view read AS OF a revision runs its current definition over the relations as they were there.",
            set_up_script: &[
                "CREATE TABLE myhistorytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 1'), (2, 'second row, 1'), (3, 'third row, 1');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-01');",
                "DELETE FROM myhistorytable;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 2'), (2, 'second row, 2'), (3, 'third row, 2');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-02');",
                "DELETE FROM myhistorytable;",
                "ALTER TABLE myhistorytable ADD COLUMN c text NOT NULL;",
                "INSERT INTO myhistorytable VALUES (1, 'first row, 3', '1'), (2, 'second row, 3', '2'), (3, 'third row, 3', '3');",
                "SELECT dolt_commit('-Am', 'test commit');",
                "SELECT dolt_branch('2019-01-03');",
                "CREATE TABLE mytable (i bigint PRIMARY KEY, s text NOT NULL);",
                "INSERT INTO mytable VALUES (1, 'first row, 1'), (2, 'second row, 2'), (3, 'third row, 3');",
                "CREATE VIEW myview1 AS SELECT * FROM myhistorytable;",
                "CREATE VIEW myview2 AS SELECT * FROM myview1 WHERE i = 1;",
                "CREATE VIEW myview3 AS SELECT i::text AS i FROM myview1 UNION SELECT s FROM myhistorytable;",
                "CREATE VIEW myview4 AS SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint) FROM myhistorytable);",
                "CREATE VIEW myview5 AS SELECT * FROM (SELECT * FROM myhistorytable WHERE i IN (SELECT DISTINCT CAST(right(s, 1) AS bigint))) AS sq;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' AND table_type = 'VIEW' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("myview1")],
                            &[T("myview2")],
                            &[T("myview3")],
                            &[T("myview4")],
                            &[T("myview5")],
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
