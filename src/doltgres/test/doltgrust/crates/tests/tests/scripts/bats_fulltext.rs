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

//! Dolt's integration-tests/bats/fulltext.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_fulltext() {
    run_scripts(&[
        ScriptTest {
            name: "fulltext: basic persistence checking",
            intent: "A text search index over two columns finds the row matching a word.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk1 numeric(20), pk2 numeric(20), v1 VARCHAR(200), v2 VARCHAR(200), PRIMARY KEY (pk1, pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '') || ' ' || coalesce(v2, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1, 'abc', 'def pqr'), (2, 1, 'ghi', 'jkl'), (3, 1, 'mno', 'mno'), (4, 1, 'stu vwx', 'xyz zyx yzx'), (5, 1, 'ghs', 'mno shg');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v2 FROM test WHERE to_tsvector('simple', coalesce(v1, '') || ' ' || coalesce(v2, '')) @@ plainto_tsquery('simple', 'jkl');",
                    expected: Expected::Rows {
                        columns: &[Column("v2", VARCHAR)],
                        rows: &[
                            &[T("jkl")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's MATCH ... AGAINST is the text search vector matched by @@ against a query")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: basic merge",
            intent: "A text search index follows a merge that deletes and adds rows, and the word counts follow.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200), v2 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '') || ' ' || coalesce(v2, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc', 'def pqr'), (2, 'ghi', 'jkl'), (3, 'mno', 'mno'), (4, 'stu vwx', 'xyz zyx yzx'), (5, 'ghs', 'mno shg');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word, ndoc FROM ts_stat('SELECT to_tsvector(''simple'', coalesce(v1, '''') || '' '' || coalesce(v2, '''')) FROM test') ORDER BY word;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4)],
                        rows: &[
                            &[T("abc"), T("1")],
                            &[T("def"), T("1")],
                            &[T("ghi"), T("1")],
                            &[T("ghs"), T("1")],
                            &[T("jkl"), T("1")],
                            &[T("mno"), T("2")],
                            &[T("pqr"), T("1")],
                            &[T("shg"), T("1")],
                            &[T("stu"), T("1")],
                            &[T("vwx"), T("1")],
                            &[T("xyz"), T("1")],
                            &[T("yzx"), T("1")],
                            &[T("zyx"), T("1")],
                        ],
                        tag: "SELECT 13",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so ts_stat counts each word's rows, as its global_count table does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Main commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (6, 'jak', 'mno'), (7, 'mno', 'bot');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Other commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word, ndoc FROM ts_stat('SELECT to_tsvector(''simple'', coalesce(v1, '''') || '' '' || coalesce(v2, '''')) FROM test') ORDER BY word;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4)],
                        rows: &[
                            &[T("abc"), T("1")],
                            &[T("bot"), T("1")],
                            &[T("def"), T("1")],
                            &[T("ghi"), T("1")],
                            &[T("ghs"), T("1")],
                            &[T("jak"), T("1")],
                            &[T("jkl"), T("1")],
                            &[T("mno"), T("3")],
                            &[T("pqr"), T("1")],
                            &[T("shg"), T("1")],
                            &[T("stu"), T("1")],
                            &[T("vwx"), T("1")],
                            &[T("xyz"), T("1")],
                            &[T("yzx"), T("1")],
                            &[T("zyx"), T("1")],
                        ],
                        tag: "SELECT 15",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so ts_stat counts each word's rows, as its global_count table does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM test WHERE to_tsvector('simple', coalesce(v1, '') || ' ' || coalesce(v2, '')) @@ plainto_tsquery('simple', 'mno') ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's MATCH ... AGAINST is the text search vector matched by @@ against a query")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: drop index, tables removed",
            intent: "A dropped text search index is gone.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DROP INDEX ... ON a table is DROP INDEX of the index")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: drop index on other branch, ff merge",
            intent: "A fast-forward merge of a branch that dropped the text search index drops it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Dropped index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
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
            name: "fulltext: drop index on other branch, no-ff merge",
            intent: "A merge of a branch that dropped the text search index drops it and keeps both branches' rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'def');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Insertion commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'ghi');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Dropped index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
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
            name: "fulltext: create index on other branch, ff merge",
            intent: "A fast-forward merge of a branch that made a text search index brings it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'ghi');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Created index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: create index on other branch, no-ff merge",
            intent: "A merge of a branch that made a text search index brings it and keeps both branches' rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'def');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Insertion commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'ghi');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Created index');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so pg_indexes lists the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
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
            name: "fulltext: merges that do not touch FT tables do not modify FT tables",
            intent: "A merge of another table's changes leaves the text search table and its word counts unchanged.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200), v2 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test USING gin (to_tsvector('simple', coalesce(v1, '') || ' ' || coalesce(v2, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE unrelated (pk numeric(20) PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abc', 'def pqr'), (2, 'ghi', 'jkl'), (3, 'mno', 'mno'), (4, 'stu vwx', 'xyz zyx yzx'), (5, 'ghs', 'mno shg');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO unrelated VALUES (6), (7), (8);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unrelated ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
                        rows: &[
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word, ndoc FROM ts_stat('SELECT to_tsvector(''simple'', coalesce(v1, '''') || '' '' || coalesce(v2, '''')) FROM test') ORDER BY word;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4)],
                        rows: &[
                            &[T("abc"), T("1")],
                            &[T("def"), T("1")],
                            &[T("ghi"), T("1")],
                            &[T("ghs"), T("1")],
                            &[T("jkl"), T("1")],
                            &[T("mno"), T("2")],
                            &[T("pqr"), T("1")],
                            &[T("shg"), T("1")],
                            &[T("stu"), T("1")],
                            &[T("vwx"), T("1")],
                            &[T("xyz"), T("1")],
                            &[T("yzx"), T("1")],
                            &[T("zyx"), T("1")],
                        ],
                        tag: "SELECT 13",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so ts_stat counts each word's rows, as its global_count table does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM unrelated WHERE pk = 7;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Main commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO unrelated VALUES (9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Other commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unrelated ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", NUMERIC)],
                        rows: &[
                            &[T("6")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word, ndoc FROM ts_stat('SELECT to_tsvector(''simple'', coalesce(v1, '''') || '' '' || coalesce(v2, '''')) FROM test') ORDER BY word;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4)],
                        rows: &[
                            &[T("abc"), T("1")],
                            &[T("def"), T("1")],
                            &[T("ghi"), T("1")],
                            &[T("ghs"), T("1")],
                            &[T("jkl"), T("1")],
                            &[T("mno"), T("2")],
                            &[T("pqr"), T("1")],
                            &[T("shg"), T("1")],
                            &[T("stu"), T("1")],
                            &[T("vwx"), T("1")],
                            &[T("xyz"), T("1")],
                            &[T("yzx"), T("1")],
                            &[T("zyx"), T("1")],
                        ],
                        tag: "SELECT 13",
                    },
                    dolt: Some(Dolt::MySqlOnly("Dolt's pseudo-index tables are its own, so ts_stat counts each word's rows, as its global_count table does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: psuedo-index tables do not show in dolt status",
            intent: "A table with a text search index shows in the status as one table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test_abc (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test_abc USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_abc VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test_abc"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: psuedo-index tables do not show in dolt diff",
            intent: "A table with a text search index shows in the diff as one table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test_abc (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test_abc USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_abc VALUES (1, 'abc');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Inserted row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test_abc"), T("public.test_abc"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test_abc"), T("public.test_abc"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fulltext: psuedo-index tables do not show in dolt schema show",
            intent: "A table with a text search index is listed as one table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test_abc (pk numeric(20) PRIMARY KEY, v1 VARCHAR(200));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON test_abc USING gin (to_tsvector('simple', coalesce(v1, '')));",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's FULLTEXT index is a GIN index over the columns' text search vector")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test_abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
