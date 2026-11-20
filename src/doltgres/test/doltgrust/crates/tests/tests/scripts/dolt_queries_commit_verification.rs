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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_commit_verification.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_commit_verification_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "test verification system variables exist and have correct defaults",
            intent: "dolt_commit_verification_groups exists and is empty by default.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, setting FROM pg_settings WHERE name = 'dolt_commit_verification_groups';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T("dolt_commit_verification_groups"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GLOBAL VARIABLES is MySQL's; pg_settings shows the setting")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test verification system variables can be set",
            intent: "dolt_commit_verification_groups takes * or a list of test groups.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '*';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, setting FROM pg_settings WHERE name = 'dolt_commit_verification_groups';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T("dolt_commit_verification_groups"), T("*")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GLOBAL VARIABLES is MySQL's; pg_settings shows the setting")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = 'unit,integration';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, setting FROM pg_settings WHERE name = 'dolt_commit_verification_groups';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT)],
                        rows: &[
                            &[T("dolt_commit_verification_groups"), T("unit,integration")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GLOBAL VARIABLES is MySQL's; pg_settings shows the setting")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "commit verification enabled - all tests pass",
            intent: "With verification on, dolt_commit runs the dolt_tests and commits when they pass.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com'), (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '2'), ('test_alice_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = ''Alice''', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit with passing tests');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "commit verification enabled - tests fail, commit aborted",
            intent: "With verification on, dolt_commit fails when a test fails and commits nothing.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com'), (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '2'), ('test_will_fail', 'integration', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit that should fail verification');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "commit verification failed: test_will_fail (Assertion failed: expected_single_value equal to 999, got 2)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--skip-verification','-m', 'skip verification');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "commit with test verification - specific test groups",
            intent: "Verification runs only the tests of the configured groups.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = 'unit';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com'), (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '2'), ('test_will_fail', 'integration', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit with unit tests only');",
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
                    query: "SET dolt_commit_verification_groups = 'integration';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '--amend', '-m', 'fail please');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "commit verification failed: test_will_fail (Assertion failed: expected_single_value equal to 999, got 2)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '--amend', '--skip-verification', '-m', 'skip the tests');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick with test verification enabled - test pass, then test fails",
            intent: "A cherry-pick runs the tests, succeeding when they pass and stopping when a later cherry-pick's test fails.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_user_count_update', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'add test');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_user_count_update';",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit_1_hash', dolt_commit('--skip-verification', '-m', 'Add Bob and update test'), FALSE);",
                "INSERT INTO users VALUES (3, 'Charlie', 'chuck@exampl.com');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit_2_hash', dolt_commit('--skip-verification', '-m', 'Add Charlie'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit_1_hash', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit_2_hash', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "commit verification failed: test_user_count_update (Assertion failed: expected_single_value equal to 2, got 3)", ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns a cherry-pick's stop at a failed commit verification as a row with an empty hash and zero counts and the failure as a warning instead of an error, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staged FROM dolt_status WHERE table_name = 'public.users';",
                    expected: Expected::Rows {
                        columns: &[Column("staged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM users;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick with test verification enabled - tests fail, leaves dirty state",
            intent: "A cherry-pick whose tests fail stops and keeps its changes for --continue or --abort.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit_hash', dolt_commit('--skip-verification', '-m', 'Add Bob but dont update test'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit_hash', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 2)", ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns a cherry-pick's stop at a failed commit verification as a row with an empty hash and zero counts and the failure as a warning instead of an error, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staged FROM dolt_status WHERE table_name = 'public.users';",
                    expected: Expected::Rows {
                        columns: &[Column("staged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 2)", ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns a cherry-pick's stop at a failed commit verification as a row with an empty hash and zero counts and the failure as a warning instead of an error, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT * FROM dolt_cherry_pick('--skip-verification', current_setting('test.commit_hash', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick with test verification enabled - fix data and --continue succeeds",
            intent: "After fixing the data a failed test checks, dolt_cherry_pick --continue succeeds.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.commit_hash', dolt_commit('--skip-verification', '-m', 'Add Bob but dont update test'), FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit_hash', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 2)", ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns a cherry-pick's stop at a failed commit verification as a row with an empty hash and zero counts and the failure as a warning instead of an error, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_users_count';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM users;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase with test verification enabled - tests pass",
            intent: "A rebase runs the tests on each commit and finishes when they pass.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "DELETE FROM users WHERE id = 1;",
                "INSERT INTO users VALUES (1, 'Zed', 'zed@example.com');",
                "SELECT dolt_commit('-am', 'drop Alice, add Zed');",
                "SELECT * FROM dolt_checkout('-b', 'feature', 'HEAD~1');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_users_count';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add Bob and update test');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "UPDATE dolt_tests SET assertion_value = '3' WHERE test_name = 'test_users_count';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add Charlie, update test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('main');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase with test verification enabled - tests fail, aborted",
            intent: "A rebase whose test fails stops with the failure.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_users_count';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Add Bob but dont update test');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 2, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns a rebase's stop at a failed commit verification as a row with status 1 and Dolt's message instead of an error, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Interactive rebase aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--skip-verification', 'main');",
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
                    query: "SELECT * FROM dolt_test_run('*');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("test_users_count"), T("unit"), T("SELECT COUNT(*) FROM users"), T("FAIL"), T("Assertion failed: expected_single_value equal to 2, got 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "interactive rebase with --skip-verification flag should persist across continue operations",
            intent: "An interactive rebase started with --skip-verification skips the tests through each --continue.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Bob but dont update test');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO users VALUES (4, 'David', 'david@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add David on main');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--interactive', '--skip-verification', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_feature; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test verification with no dolt_tests errors",
            intent: "Verification with no dolt_tests table reports it.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit without dolt_tests table');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "failed to run dolt_test_run for group *: could not find tests for argument: *", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test verification with mixed test groups - only specified groups run",
            intent: "With tests in several groups, verification runs only the configured ones.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = 'unit';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com'), (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_unit', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '2'), ('test_users_integration', 'integration', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit with unit tests only - should pass');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test verification error message includes test details",
            intent: "A failed verification's error names the test and why it failed.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com'), (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_specific_failure', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Commit with specific test failure');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "commit verification failed: test_specific_failure (Assertion failed: expected_single_value equal to 999, got 2)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with test verification enabled - tests pass",
            intent: "A merge runs the tests and commits when they pass.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_alice_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = ''Alice''', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_bob_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = \"Bob\"', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Bob');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
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
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with test verification enabled - tests fail, leaves dirty state",
            intent: "A merge whose tests fail keeps its merged state for fixing.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_will_fail', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Initial commit with failing test');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Bob');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to force non-FF merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("commit verification failed: test_will_fail (Assertion failed: expected_single_value equal to 999, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staged FROM dolt_status WHERE table_name = 'public.users';",
                    expected: Expected::Rows {
                        columns: &[Column("staged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with test verification enabled - fix data and dolt_commit succeeds",
            intent: "After fixing the data a failed test checks, dolt_commit finishes the merge.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Bob but dont update test');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to force non-FF merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Complete merge without fixing');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 3)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staged FROM dolt_status WHERE table_name = 'public.users';",
                    expected: Expected::Rows {
                        columns: &[Column("staged", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_tests SET assertion_value = '3' WHERE test_name = 'test_users_count';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Complete merge after fixing test');",
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
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
                    query: "SELECT COUNT(*) FROM users;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with --skip-verification flag bypasses verification",
            intent: "dolt_merge --skip-verification merges without running the tests.",
            set_up_script: &[
                "SET dolt_commit_verification_groups = '*';",
                "CREATE TABLE users (id int PRIMARY KEY, name VARCHAR(100) NOT NULL, email VARCHAR(100));",
                "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');",
                "INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_will_fail', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Initial commit with failing test');",
                "SELECT * FROM dolt_checkout('-b', 'feature');",
                "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Bob');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to force non-FF merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--skip-verification', 'feature');",
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
                    query: "SELECT * FROM dolt_test_run('*');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("test_will_fail"), T("unit"), T("SELECT COUNT(*) FROM users"), T("FAIL"), T("Assertion failed: expected_single_value equal to 999, got 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_commit_verification_groups = '';",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET GLOBAL is the session's SET here, since the Dolt harness only needs the setting for this session")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
