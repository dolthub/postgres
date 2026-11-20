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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_help.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_help_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_help arguments are valid json",
            intent: "Every dolt_help row's arguments are JSON.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_help WHERE arguments IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("json_valid is MySQL's; a jsonb column holds only valid JSON, so the check is that every row has arguments")),
                    ..A
                },
            ],
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help synopsis remains the same after multiple queries",
            intent: "dolt_help's synopsis for a procedure is the same on every read.",
            set_up_script: &[
                "SELECT set_config('test.InitialSynopsis', ((SELECT synopsis FROM dolt_help WHERE name='dolt_branch'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_help WHERE name='dolt_branch' AND synopsis!=current_setting('test.InitialSynopsis', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("type", TEXT), Column("synopsis", TEXT), Column("short_description", TEXT), Column("long_description", TEXT), Column("arguments", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help names are correct",
            intent: "dolt_help lists Dolt's procedures by name.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_help;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("dolt_add")],
                            &[T("dolt_reset")],
                            &[T("dolt_clean")],
                            &[T("dolt_commit")],
                            &[T("dolt_branch")],
                            &[T("dolt_checkout")],
                            &[T("dolt_merge")],
                            &[T("dolt_conflicts_resolve")],
                            &[T("dolt_cherry_pick")],
                            &[T("dolt_revert")],
                            &[T("dolt_clone")],
                            &[T("dolt_fetch")],
                            &[T("dolt_pull")],
                            &[T("dolt_push")],
                            &[T("dolt_remote")],
                            &[T("dolt_backup")],
                            &[T("dolt_tag")],
                            &[T("dolt_gc")],
                            &[T("dolt_stash")],
                            &[T("dolt_rebase")],
                            &[T("dolt_rm")],
                        ],
                        tag: "SELECT 21",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help types are correct",
            intent: "dolt_help gives each procedure's type.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT type FROM dolt_help WHERE name='dolt_rebase';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("procedure")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT type FROM dolt_help WHERE name='dolt_gc';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("procedure")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT type FROM dolt_help WHERE name='dolt_tag';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("procedure")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help synopses are correct",
            intent: "dolt_help's synopses describe each procedure's usage.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_conflicts_resolve' AND synopsis LIKE '%dolt conflicts resolve%table%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_merge' AND synopsis LIKE '%dolt merge%branch%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_branch' AND synopsis LIKE '%dolt branch%--list%';",
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
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help short descriptions are correct",
            intent: "dolt_help gives each procedure's short description.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT short_description FROM dolt_help WHERE name='dolt_clean';",
                    expected: Expected::Rows {
                        columns: &[Column("short_description", TEXT)],
                        rows: &[
                            &[T("Deletes untracked working tables")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT short_description FROM dolt_help WHERE name='dolt_checkout';",
                    expected: Expected::Rows {
                        columns: &[Column("short_description", TEXT)],
                        rows: &[
                            &[T("Switch branches or restore working tree tables")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT short_description FROM dolt_help WHERE name='dolt_fetch';",
                    expected: Expected::Rows {
                        columns: &[Column("short_description", TEXT)],
                        rows: &[
                            &[T("Download objects and refs from another repository")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help long descriptions are correct",
            intent: "dolt_help gives each procedure's long description.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_add' AND long_description LIKE '%updates the list of tables%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_cherry_pick' AND long_description LIKE '%Applies the changes from an existing commit%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_revert' AND long_description LIKE '%Removes the changes made in a commit%';",
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
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
        ScriptTest {
            name: "dolt_help arguments are correct",
            intent: "dolt_help lists each procedure's options with their descriptions.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_commit' AND arguments LIKE '%--amend%Amend previous commit%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_commit' AND arguments LIKE '%--skip-empty%Only create a commit if there are staged changes%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_rebase' AND arguments LIKE '%--continue%Continue an interactive rebase%';",
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
                    query: "SELECT count(*) FROM dolt_help WHERE name='dolt_revert' AND arguments LIKE '%--author%Specify an explicit author%';",
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
            skip: Some("dolt_help is deferred by design (the owner, 2026-10-31)"),
            ..S
        },
    ]);
}
