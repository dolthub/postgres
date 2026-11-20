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

//! Dolt's integration-tests/bats/commit_verification.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_commit_verification() {
    run_scripts(&[
        ScriptTest {
            name: r#"commit_verification: system variables can be set"#,
            intent: r#"dolt_commit_verification_groups takes a value that reads back."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_commit_verification_groups = '*';"#,
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("SET @@PERSIST is MySQL's, so the port sets the session's setting")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SHOW dolt_commit_verification_groups;"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit_verification_groups", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SHOW",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GLOBAL VARIABLES LIKE is MySQL's, so the port uses SHOW")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: commit with tests enabled - all tests pass"#,
            intent: r#"A commit whose tests pass is made."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_alice_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = ''Alice''', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Commit with passing tests');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: abort commit, then skip verification to bypass"#,
            intent: r#"A commit whose test fails is refused, naming the test, and --skip-verification makes it."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_will_fail', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Commit that should fail verification');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"commit verification failed: test_will_fail (Assertion failed: expected_single_value equal to 999, got 1)"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--skip-verification', '-m', 'Skip verification commit');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: specific test groups - only specified groups run"#,
            intent: r#"Only the tests of the named groups run, so a failing test of another group doesn't refuse the commit."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = 'unit';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_unit', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_integration', 'integration', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');"#,
                r#"SELECT dolt_add('.');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Commit with unit tests only');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: merge with tests enabled - tests pass"#,
            intent: r#"A merge whose tests pass is made."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_alice_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = ''Alice''', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_bob_exists', 'unit', 'SELECT COUNT(*) FROM users WHERE name = ''Bob''', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add Bob');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT hash IS NOT NULL, fast_forward, conflicts FROM dolt_merge('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("fast_forward", INT8), Column("conflicts", INT8)],
                        rows: &[
                            &[T("t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: merge with tests enabled - tests fail, can abort and retry with --skip-verification"#,
            intent: r#"A merge whose test fails stops in progress, naming the test, and after an abort --skip-verification makes it."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_will_fail', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '999');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Initial commit with failing test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("commit verification failed: test_will_fail (Assertion failed: expected_single_value equal to 999, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns the merge's row with the verification failure as its message, where Dolt raises it, since an error would roll back the stop that keeps the merge in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge --abort is a CLI command, which dolt_merge('--abort') is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT hash IS NOT NULL, fast_forward, conflicts, message FROM dolt_merge('--skip-verification', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("t"), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: cherry-pick with tests enabled - tests pass"#,
            intent: r#"A cherry-pick whose tests pass with the picked commit's test change is made."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_user_count_update', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_user_count_update';"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob and update test');"#,
                r#"SELECT set_config('test.hash', dolt_hashof('HEAD'), false);"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT hash IS NOT NULL, data_conflicts FROM dolt_cherry_pick(current_setting('test.hash'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("data_conflicts", INT8)],
                        rows: &[
                            &[T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, which dolt_cherry_pick() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: cherry-pick with tests enabled - tests fail, can abort and retry with --skip-verification"#,
            intent: r#"A cherry-pick whose test fails stops in progress, naming the test, and after an abort --skip-verification makes it."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob but don''t update test');"#,
                r#"SELECT set_config('test.hash', dolt_hashof('HEAD'), false);"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick(current_setting('test.hash'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"commit verification failed: test_users_count (Assertion failed: expected_single_value equal to 1, got 2)"#, ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns the cherry-pick's row with the verification failure as a WARNING, where Dolt raises it, since an error would roll back the stop that keeps the cherry-pick in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT hash IS NOT NULL, data_conflicts FROM dolt_cherry_pick('--skip-verification', current_setting('test.hash'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("data_conflicts", INT8)],
                        rows: &[
                            &[T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: rebase with tests enabled - tests pass"#,
            intent: r#"A rebase whose commits' tests pass is made."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('test_users_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"UPDATE users SET name = 'Zed' WHERE id = 1;"#,
                r#"SELECT dolt_commit('-am', 'Update Alice to Zed');"#,
                r#"SELECT dolt_checkout('-b', 'feature', 'HEAD~1');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'test_users_count';"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add Bob and update test');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"UPDATE dolt_tests SET assertion_value = '3' WHERE test_name = 'test_users_count';"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add Charlie, update test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, which dolt_rebase() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: rebase with tests enabled - tests fail, can abort and restart with --skip-verification"#,
            intent: r#"A rebase whose commit's test fails stops, naming the test, and after an abort --skip-verification makes it."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'user_count';"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add Bob and update test');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie');"#,
                r#"SELECT dolt_checkout('feature');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("commit verification failed: user_count (Assertion failed: expected_single_value equal to 2, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns status 1 with the verification failure as the message, where Dolt raises it, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--abort');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('--skip-verification', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: cherry-pick verification failure halts with dirty state preserved"#,
            intent: r#"A cherry-pick whose test fails stays in progress with its change staged, --continue fails the same way, and --abort restores the branch."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob without updating test');"#,
                r#"SELECT set_config('test.hash', dolt_hashof('HEAD'), false);"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick(current_setting('test.hash'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 2)"#, ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns the cherry-pick's row with the verification failure as a WARNING, where Dolt raises it, since an error would roll back the stop that keeps the cherry-pick in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 2)"#, ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns the cherry-pick's row with the verification failure as a WARNING, where Dolt raises it, since an error would roll back the stop that keeps the cherry-pick in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM users ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("email", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Alice"), T("alice@example.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: cherry-pick fix data then --continue succeeds"#,
            intent: r#"After the failing test is fixed and staged, --continue makes the cherry-pick."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob without updating test');"#,
                r#"SELECT set_config('test.hash', dolt_hashof('HEAD'), false);"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick(current_setting('test.hash'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 2)"#, ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust returns the cherry-pick's row with the verification failure as a WARNING, where Dolt raises it, since an error would roll back the stop that keeps the cherry-pick in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_tests SET assertion_value = '2' WHERE test_name = 'user_count';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT hash IS NOT NULL, data_conflicts FROM dolt_cherry_pick('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("data_conflicts", INT8)],
                        rows: &[
                            &[T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM users ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("email", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Alice"), T("alice@example.com")],
                            &[T("2"), T("Bob"), T("bob@example.com")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: merge verification failure halts with dirty state preserved"#,
            intent: r#"A merge whose test fails stays in progress with its change staged, and fixing the test lets the merge commit."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"SELECT dolt_checkout('-b', 'feature');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob without updating test');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to force non-FF merge');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns the merge's row with the verification failure as its message, where Dolt raises it, since an error would roll back the stop that keeps the merge in progress (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_tests SET assertion_value = '3' WHERE test_name = 'user_count';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Complete merge after fixing test');"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt commit is a CLI command, which dolt_commit() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: rebase verification failure halts with dirty state preserved"#,
            intent: r#"A rebase whose commit's test fails stops with the commit's change staged, and --abort ends the rebase."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to main');"#,
                r#"SELECT dolt_checkout('-b', 'b1', 'HEAD~1');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob without updating test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns status 1 with the verification failure as the message, where Dolt raises it, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--abort');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status's rebase in progress line is the CLI's, so the port checks the session is back on its branch")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"commit_verification: rebase fix data then --continue succeeds"#,
            intent: r#"After the failing test is fixed and staged, --continue finishes the rebase."#,
            set_up_script: &[
                r#"CREATE TABLE users (id int PRIMARY KEY, name varchar(100) NOT NULL, email varchar(100));"#,
                r#"INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                r#"SET dolt_commit_verification_groups = '*';"#,
                r#"INSERT INTO dolt_tests (test_name, test_group, test_query, assertion_type, assertion_comparator, assertion_value) VALUES ('user_count', 'unit', 'SELECT COUNT(*) FROM users', 'expected_single_value', '==', '1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add verification test');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Charlie to main');"#,
                r#"SELECT dolt_checkout('b1');"#,
                r#"INSERT INTO users VALUES (2, 'Bob', 'bob@example.com');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--skip-verification', '-m', 'Add Bob without updating test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("commit verification failed: user_count (Assertion failed: expected_single_value equal to 1, got 3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns status 1 with the verification failure as the message, where Dolt raises it, since an error would roll back the stop (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_tests SET assertion_value = '3' WHERE test_name = 'user_count';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
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
