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

//! Dolt's integration-tests/bats/docs.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_docs() {
    run_scripts(&[
        ScriptTest {
            name: "docs: docs can be created from SQL",
            intent: "dolt_docs can't be created as a table of that name, and comes into being when a doc is inserted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE dolt_docs (x int);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "Invalid table name dolt_docs. Table names beginning with `dolt_` are reserved for internal use", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dolt_docs (doc_name varchar(1023) NOT NULL, doc_text text, PRIMARY KEY (doc_name));",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "Invalid table name dolt_docs. Table names beginning with `dolt_` are reserved for internal use", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('README.md', 'this is a README');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_docs;",
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T("README.md"), T("this is a README")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: doc read creates dolt_docs table"#,
            intent: r#"Uploading a doc, even twice, makes dolt_docs, which shows as a new table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_docs;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload is a CLI command that writes a file's text as a doc, which an upsert into dolt_docs does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload is a CLI command that writes a file's text as a doc, which an upsert into dolt_docs does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: doc read outputs doc correctly"#,
            intent: r#"A doc reads back exactly as it was written."#,
            set_up_script: &[
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT doc_text = E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n' FROM dolt_docs WHERE doc_name = 'LICENSE.md';"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt docs print is a CLI command, so the port compares the doc's text in dolt_docs")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: docs can be staged"#,
            intent: r#"An uploaded doc can be staged."#,
            set_up_script: &[
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: doc can be committed"#,
            intent: r#"A staged doc commits, leaving a clean working set."#,
            set_up_script: &[
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'added a license file');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: docs are available from SQL"#,
            intent: r#"dolt_docs is readable before any doc exists and lists an uploaded doc."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_docs;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload is a CLI command that writes a file's text as a doc, which an upsert into dolt_docs does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT doc_name FROM dolt_docs;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR)],
                        rows: &[
                            &[T("LICENSE.md")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"docs: docs diff"#,
            intent: r#"A doc changed after a commit diffs against the committed text."#,
            set_up_script: &[
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'added LICENSE');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', E'        DO WHAT THE F*CK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE F*CK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE F*CK YOU WANT TO.\n') ON CONFLICT (doc_name) DO UPDATE SET doc_text = excluded.doc_text;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload is a CLI command that writes a file's text as a doc, which an upsert into dolt_docs does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_doc_text = E'        DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE FUCK YOU WANT TO.\n', to_doc_text = E'        DO WHAT THE F*CK YOU WANT TO PUBLIC LICENSE\n                    Version 2, December 2004\n\n Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>\n\n Everyone is permitted to copy and distribute verbatim or modified\n copies of this license document, and changing it is allowed as long\n as the name is changed.\n\n            DO WHAT THE F*CK YOU WANT TO PUBLIC LICENSE\n   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION\n\n  0. You just DO WHAT THE F*CK YOU WANT TO.\n', dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'dolt_docs');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt docs diff is a CLI command, so the port reads the doc's diff from dolt_diff")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
