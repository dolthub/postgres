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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_query_catalog.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_query_catalog_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "can insert into dolt query catalog",
            intent: "A saved query can be inserted into dolt_query_catalog.",
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_query_catalog VALUES ('show', 1, 'show', 'show tables;', '');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can drop dolt query catalog, cannot drop twice",
            intent: "dolt_query_catalog can be dropped once, and a second drop fails.",
            set_up_script: &[
                "INSERT INTO dolt_query_catalog VALUES ('show', 1, 'show', 'show tables;', '');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE dolt_query_catalog;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE dolt_query_catalog;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("a versioned system table's definition is never stored, so DROP TABLE empties it and a second DROP empties it again, where Dolt's second DROP finds no table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can run delete from on dolt query catalog",
            intent: "DELETE works on dolt_query_catalog.",
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_query_catalog;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select from dolt_query_catalog",
            intent: "dolt_query_catalog returns its saved queries, filtered by display order.",
            set_up_script: &[
                "INSERT INTO dolt_query_catalog VALUES ('show', 1, 'show', 'show tables;', 'my message');",
                "INSERT INTO dolt_query_catalog VALUES ('get commits', 2, 'get commits', 'select * from dolt_commits;', '');",
                "INSERT INTO dolt_query_catalog VALUES ('get branches', 3, 'get branches', 'select * from dolt_branches;', '');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("show"), T("1"), T("show"), T("show tables;"), T("my message")],
                            &[T("get commits"), T("2"), T("get commits"), T("select * from dolt_commits;"), T("")],
                            &[T("get branches"), T("3"), T("get branches"), T("select * from dolt_branches;"), T("")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog WHERE display_order = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("get commits"), T("2"), T("get commits"), T("select * from dolt_commits;"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can replace row in dolt_query_catalog",
            intent: "A saved query with an existing id replaces the old one.",
            set_up_script: &[
                "INSERT INTO dolt_query_catalog VALUES ('test', 1, 'test', 'show tables;', '');",
                "INSERT INTO dolt_query_catalog VALUES ('test2', 2, 'test2', 'select * from dolt_commits;', '');",
                "INSERT INTO dolt_query_catalog VALUES ('test', 1, 'new name', 'describe dolt_query_catalog;', 'a new message') ON CONFLICT (id) DO UPDATE SET display_order = excluded.display_order, name = excluded.name, query = excluded.query, description = excluded.description;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("test"), T("1"), T("new name"), T("describe dolt_query_catalog;"), T("a new message")],
                            &[T("test2"), T("2"), T("test2"), T("select * from dolt_commits;"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can update dolt query catalog",
            intent: "UPDATE works on dolt_query_catalog.",
            set_up_script: &[
                "INSERT INTO dolt_query_catalog VALUES ('show', 1, 'show', 'show tables;', '');",
                "UPDATE dolt_query_catalog SET display_order = display_order + 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog;",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("show"), T("2"), T("show"), T("show tables;"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can use 'as of' on dolt_query_catalog",
            intent: "dolt_query_catalog read AS OF a commit shows the saved queries committed then.",
            set_up_script: &[
                "INSERT INTO dolt_query_catalog VALUES ('show', 1, 'show', 'show tables;', '');",
                "SELECT dolt_commit('-A','-m', 'first commit');",
                "INSERT INTO dolt_query_catalog VALUES ('select', 2, 'select', 'select * from dolt_query_catalog;', '');",
                "SELECT dolt_commit('-A', '-m', 'second commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog AS OF 'HEAD~2';",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog AS OF 'HEAD~1';",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("show"), T("1"), T("show"), T("show tables;"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_catalog AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("id", TEXT), Column("display_order", INT4), Column("name", TEXT), Column("query", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("show"), T("1"), T("show"), T("show tables;"), T("")],
                            &[T("select"), T("2"), T("select"), T("select * from dolt_query_catalog;"), T("")],
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
