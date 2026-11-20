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

//! Dolt's integration-tests/bats/ignore.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_ignore() {
    run_scripts(&[
        ScriptTest {
            name: "ignore: simple matches",
            intent: "dolt_add stages the new tables no pattern ignores and leaves out the one a pattern ignores, which dolt_status then leaves out.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE dontignore (pk int);",
                "CREATE TABLE nomatch (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.dontignore"), T("t"), T("new table")],
                            &[T("public.nomatch"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: specific overrides",
            intent: "Where an ignoring and a keeping pattern both match a name, the more specific one decides.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE please_ignore (pk int);",
                "CREATE TABLE please_ignore_too (pk int);",
                "CREATE TABLE do_not_ignore (pk int);",
                "CREATE TABLE commit_me (pk int);",
                "CREATE TABLE commit_me_not (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.commit_me"), T("t"), T("new table")],
                            &[T("public.do_not_ignore"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: conflict",
            intent: "A name that an ignoring and a keeping pattern match with neither more specific fails staging, naming both patterns.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE commit_ignore (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the table commit_ignore matches conflicting patterns in dolt_ignore:\nignored:     *_ignore\nnot ignored: commit_*", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: question mark",
            intent: "A question mark matches one character, so test? ignores test1 and test?* keeps test11.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE test (pk int);",
                "CREATE TABLE test1 (pk int);",
                "CREATE TABLE test11 (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test11"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: allow staging ignored tables if 'add --force' is supplied",
            intent: "dolt_add with --force stages the tables the patterns ignore, dolt_ignore itself included.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A', '--force');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_ignore"), T("t"), T("new table")],
                            &[T("public.ignoreme"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: don't auto-stage ignored tables",
            intent: "dolt_commit -A leaves out the tables the patterns ignore, so the commit holds only the others.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE nomatch (pk int);",
                "SELECT dolt_commit('-m', 'commit1', '-A');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD~', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.nomatch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt status doesn't show ignored tables when --ignored is not supplied",
            intent: "dolt_status leaves out the new tables the patterns ignore.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE nomatch (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.nomatch"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt status shows ignored tables when --ignored is not supplied",
            intent: "dolt_status_ignored, the SQL form of dolt status --ignored, lists the ignored new tables too and marks them.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE nomatch (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, ignored FROM dolt_status_ignored ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("dolt_ignore"), T("t")],
                            &[T("public.ignoreme"), T("t")],
                            &[T("public.nomatch"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: don't display new but ignored tables in dolt diff",
            intent: "A diff of the working set against the staged root leaves out the new tables the patterns ignore.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE nomatch (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('STAGED', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.nomatch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: don't display new but ignored tables in reverse diff",
            intent: "The reverse diff leaves them out too.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE nomatch (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM dolt_diff_summary('WORKING', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.nomatch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: DO display modified ignored tables in dolt diff after staging",
            intent: "A staged table the patterns ignore is tracked, so its later changes show in the diff.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "SELECT dolt_add('--force', 'ignoreme');",
                "INSERT INTO ignoreme VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('STAGED', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.ignoreme")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: DO display modified ignored tables in reverse diff after staging",
            intent: "The reverse diff shows them too.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "SELECT dolt_add('--force', 'ignoreme');",
                "INSERT INTO ignoreme VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM dolt_diff_summary('WORKING', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.ignoreme")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: DO display modified ignored tables in dolt diff after committing",
            intent: "A committed table the patterns ignore is tracked, so its later changes show in the diff.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "SELECT dolt_add('--force', 'ignoreme');",
                "SELECT dolt_commit('-m', 'commit1');",
                "INSERT INTO ignoreme VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('STAGED', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.ignoreme")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: detect when equivalent patterns have different values",
            intent: "Patterns that differ only by repeated or spelled-differently wildcards are equivalent, so an ignoring and a keeping one conflict.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "INSERT INTO dolt_ignore VALUES ('**_test', true), ('*_test', false), ('*_foo', true), ('%_foo', false);",
                "CREATE TABLE a_test (pk int);",
                "CREATE TABLE a_foo (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the table a_foo matches conflicting patterns in dolt_ignore:\nignored:     *_foo\nnot ignored: %_foo", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('a_test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the table a_test matches conflicting patterns in dolt_ignore:\nignored:     **_test\nnot ignored: *_test", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: allow using dolt_ignore with AS OF",
            intent: "dolt_ignore reads as of the working set, the staged root, a commit, or a branch, and as empty where it was never written.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "SELECT dolt_branch('start');",
                "INSERT INTO dolt_ignore VALUES ('dolt_ignore', false);",
                "INSERT INTO dolt_ignore VALUES ('test1', true);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'Insert into dolt_ignore');",
                "INSERT INTO dolt_ignore VALUES ('test2', true);",
                "SELECT dolt_add('dolt_ignore');",
                "INSERT INTO dolt_ignore VALUES ('test3', true);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'WORKING' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
                        rows: &[
                            &[T("test1")],
                            &[T("test2")],
                            &[T("test3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'STAGED' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
                        rows: &[
                            &[T("test1")],
                            &[T("test2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'HEAD' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
                        rows: &[
                            &[T("test1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'main' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
                        rows: &[
                            &[T("test1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'start' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pattern FROM dolt_ignore AS OF 'HEAD^' WHERE pattern IN ('test1', 'test2', 'test3') ORDER BY pattern;",
                    expected: Expected::Rows {
                        columns: &[Column("pattern", TEXT)],
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
            name: "ignore: dolt_diff_summary respects dolt_ignore patterns",
            intent: "dolt_diff_summary against the working set leaves out a new table the patterns ignore.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE initial_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add initial table');",
                "INSERT INTO dolt_ignore VALUES ('should_ignore_new', true);",
                "CREATE TABLE should_ignore_new (pk int PRIMARY KEY);",
                "CREATE TABLE should_not_ignore_new (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.should_not_ignore_new")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt_diff_summary respects wildcard ignore patterns",
            intent: "A wildcard pattern leaves out every new table it matches.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE initial (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add initial table');",
                "INSERT INTO dolt_ignore VALUES ('temp_*', true);",
                "CREATE TABLE temp_new_table (pk int PRIMARY KEY);",
                "CREATE TABLE regular_new_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.regular_new_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt_diff_summary respects ignore patterns for dropped tables",
            intent: "A dropped table the patterns ignore is left out as a new one is.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE will_be_ignored (pk int PRIMARY KEY);",
                "CREATE TABLE will_not_be_ignored (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add tables to be dropped');",
                "INSERT INTO dolt_ignore VALUES ('will_be_ignored', true);",
                "DROP TABLE will_be_ignored;",
                "DROP TABLE will_not_be_ignored;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING');",
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
                    query: "SELECT from_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.will_not_be_ignored"), T("dropped")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt_diff_summary specific table query respects ignore patterns",
            intent: "Asking for an ignored new table by name finds nothing, and another new table is found.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE initial_table (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add initial table');",
                "INSERT INTO dolt_ignore VALUES ('ignored_table_new', true);",
                "CREATE TABLE ignored_table_new (pk int PRIMARY KEY);",
                "CREATE TABLE not_ignored_table (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING', 'ignored_table_new');",
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
                    query: "SELECT COUNT(*) FROM dolt_diff_summary('HEAD', 'WORKING', 'not_ignored_table');",
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
            ..S
        },
        ScriptTest {
            name: "ignore: dolt show should display committed tables even after they are added to dolt_ignore",
            intent: "A table committed before a pattern ignored it still shows in its commit's diff, while a new table the pattern ignores stays out of the working set's.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE the_table (pk INT PRIMARY KEY, name TEXT);",
                "SELECT dolt_add('the_table');",
                "SELECT dolt_commit('-m', 'add the_table');",
                "INSERT INTO dolt_ignore VALUES ('the_table', true);",
                "SELECT dolt_add('--force', 'dolt_ignore');",
                "SELECT dolt_commit('-m', 'ignore the_table');",
                "CREATE TABLE new_ignore (pk INT PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE another_table (pk INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('another_table');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'add another_table') IS NOT NULL AS committed;",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD~3', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.the_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: dolt diff should display committed tables even after they are added to dolt_ignore",
            intent: "A table committed before a pattern ignored it still shows in its commit's diff, while a new table the pattern ignores stays out of the working set's.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE the_table (pk INT PRIMARY KEY, name TEXT);",
                "SELECT dolt_add('the_table');",
                "SELECT dolt_commit('-m', 'add the_table');",
                "INSERT INTO dolt_ignore VALUES ('the_table', true);",
                "SELECT dolt_add('--force', 'dolt_ignore');",
                "SELECT dolt_commit('-m', 'ignore the_table');",
                "CREATE TABLE new_ignore (pk INT PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE another_table (pk INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('another_table');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'add another_table') IS NOT NULL AS committed;",
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
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD~3', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT)],
                        rows: &[
                            &[T("public.the_table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: don't stash ignored tables",
            intent: "Stashing the untracked tables leaves out the ones the patterns ignore, so with only those there is nothing to stash.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'stash1', '--include-untracked');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No local changes to save", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: error when trying to stash table with dolt_ignore conflict",
            intent: "Stashing the untracked tables fails for a table conflicting patterns match, as staging does.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE commit_ignore (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'stash1', '--include-untracked');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the table commit_ignore matches conflicting patterns in dolt_ignore:\nignored:     *_ignore\nnot ignored: commit_*", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: stash ignored and untracked tables when --all is passed",
            intent: "Stashing with --all takes the ignored and the untracked tables, and popping the stash brings both back.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE ignoreme (pk int);",
                "CREATE TABLE dontignore (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'stash1', '--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'stash1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.dontignore"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ignore: stash table with dolt_ignore conflict when --all is passed",
            intent: "Stashing with --all takes a table conflicting patterns match too, and popping the stash brings the conflict back.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('ignoreme', true), ('dontignore', false), ('*_ignore', true), ('do_not_ignore', false), ('%_ignore_too', true), ('commit_*', false), ('commit_me_not', true), ('test*', true), ('test?*', false), ('test?', true);",
                "CREATE TABLE commit_ignore (pk int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'stash1', '--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'stash1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the table commit_ignore matches conflicting patterns in dolt_ignore:\nignored:     *_ignore\nnot ignored: commit_*", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
