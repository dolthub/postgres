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

//! Dolt's go/libraries/doltcore/sqle/enginetest/branch_control_test.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_branch_control_block_tests() {
    run_scripts(&[
        ScriptTest {
            name: "INSERT",
            intent: "INSERT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSERT on branch db",
            intent: "INSERT on branch db is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPLACE",
            intent: "REPLACE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2) ON CONFLICT (\"pk\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\";",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2) ON CONFLICT (\"pk\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\";",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE",
            intent: "UPDATE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET pk = 2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET pk = 2;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE on branch db",
            intent: "UPDATE on branch db is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/other\".public.test SET pk = 2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/other\".public.test SET pk = 2;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DELETE",
            intent: "DELETE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk >= 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk >= 0;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DELETE from branch table",
            intent: "DELETE from branch table is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/other\".public.test WHERE pk >= 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/other\".public.test WHERE pk >= 0;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TRUNCATE",
            intent: "TRUNCATE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE AUTO_INCREMENT",
            intent: "ALTER TABLE AUTO_INCREMENT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE test2(pk bigint PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY);",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER TABLE test2 OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ALTER COLUMN pk RESTART WITH 20;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ALTER COLUMN pk RESTART WITH 20;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD CHECK",
            intent: "ALTER TABLE ADD CHECK is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP CHECK",
            intent: "ALTER TABLE DROP CHECK is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT check_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT check_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ALTER COLUMN SET DEFAULT",
            intent: "ALTER TABLE ALTER COLUMN SET DEFAULT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 SET DEFAULT (5);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 SET DEFAULT (5);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ALTER COLUMN DROP DEFAULT",
            intent: "ALTER TABLE ALTER COLUMN DROP DEFAULT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test ALTER COLUMN v1 SET DEFAULT (5);",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD FOREIGN KEY",
            intent: "ALTER TABLE ADD FOREIGN KEY is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON test (v1);",
                "CREATE TABLE test2 (pk bigint PRIMARY KEY, v1 bigint UNIQUE);",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER TABLE test2 OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ADD CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES test (pk);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ADD CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES test (pk);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP FOREIGN KEY",
            intent: "ALTER TABLE DROP FOREIGN KEY is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON test (v1);",
                "CREATE TABLE test2 (pk bigint PRIMARY KEY, v1 bigint UNIQUE, CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES test (pk));",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER TABLE test2 OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 DROP CONSTRAINT fk_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 DROP CONSTRAINT fk_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD INDEX",
            intent: "ALTER TABLE ADD INDEX is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test (v1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test (v1);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP INDEX",
            intent: "ALTER TABLE DROP INDEX is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON test (v1);",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME INDEX",
            intent: "ALTER TABLE RENAME INDEX is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON test (v1);",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_v1_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX idx_v1 RENAME TO idx_v1_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD PRIMARY KEY",
            intent: "ALTER TABLE ADD PRIMARY KEY is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE test2 (v1 bigint, v2 bigint);",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER TABLE test2 OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ADD PRIMARY KEY (v1, v2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ADD PRIMARY KEY (v1, v2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP PRIMARY KEY",
            intent: "ALTER TABLE DROP PRIMARY KEY is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT test_pkey;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT test_pkey;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME",
            intent: "ALTER TABLE RENAME is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO test_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO test_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RENAME TABLE",
            intent: "RENAME TABLE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO test_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO test_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD COLUMN",
            intent: "ALTER TABLE ADD COLUMN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN v2 bigint;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN v2 bigint;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP COLUMN",
            intent: "ALTER TABLE DROP COLUMN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE CHANGE COLUMN",
            intent: "ALTER TABLE CHANGE COLUMN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v1 TO v1_new; ALTER TABLE test ALTER COLUMN v1_new TYPE bigint, ALTER COLUMN v1_new DROP NOT NULL, ALTER COLUMN v1_new DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v1 TO v1_new; ALTER TABLE test ALTER COLUMN v1_new TYPE bigint, ALTER COLUMN v1_new DROP NOT NULL, ALTER COLUMN v1_new DROP DEFAULT;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE MODIFY COLUMN",
            intent: "ALTER TABLE MODIFY COLUMN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE smallint, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE smallint, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME COLUMN",
            intent: "ALTER TABLE RENAME COLUMN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX",
            intent: "CREATE INDEX is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test (v1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON test (v1);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP INDEX",
            intent: "DROP INDEX is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON test (v1);",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE VIEW",
            intent: "CREATE VIEW is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW view_1 AS SELECT * FROM TEST;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW view_1 AS SELECT * FROM TEST;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP VIEW",
            intent: "DROP VIEW is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE VIEW view_1 AS SELECT * FROM TEST;",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER VIEW view_1 OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP VIEW view_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW view_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE TRIGGER",
            intent: "CREATE TRIGGER is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP TRIGGER",
            intent: "DROP TRIGGER is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TRIGGER \"trigger_1\" ON \"test\";",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER \"trigger_1\" ON \"test\";",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE",
            intent: "CREATE TABLE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE LIKE",
            intent: "CREATE TABLE LIKE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (LIKE test INCLUDING ALL);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (LIKE test INCLUDING ALL);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE AS SELECT",
            intent: "CREATE TABLE AS SELECT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 AS SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 AS SELECT * FROM test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP TABLE",
            intent: "DROP TABLE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE test OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE PROCEDURE",
            intent: "CREATE PROCEDURE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE testabc(x double precision, y double precision) LANGUAGE sql AS $$ SELECT x*y $$;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE testabc(x double precision, y double precision) LANGUAGE sql AS $$ SELECT x*y $$;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP PROCEDURE",
            intent: "DROP PROCEDURE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE PROCEDURE testabc(x double precision, y double precision) LANGUAGE sql AS $$ SELECT x*y $$;",
                "ALTER TABLE test OWNER TO testuser;",
                "ALTER PROCEDURE testabc OWNER TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP PROCEDURE testabc;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PROCEDURE testabc;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSERT SELECT",
            intent: "INSERT SELECT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test SELECT pk + 100, v1 FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test SELECT pk + 100, v1 FROM test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INSERT ON DUPLICATE KEY UPDATE",
            intent: "INSERT ON DUPLICATE KEY UPDATE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1)  ON CONFLICT (\"pk\") DO UPDATE SET v1 = 50;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1)  ON CONFLICT (\"pk\") DO UPDATE SET v1 = 50;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE with JOIN",
            intent: "UPDATE with JOIN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test t1 SET v1 = 5 FROM test t2 WHERE t1.pk = t2.pk;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test t1 SET v1 = 5 FROM test t2 WHERE t1.pk = t2.pk;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DELETE with JOIN",
            intent: "DELETE with JOIN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM test t1 USING test t2 WHERE t1.pk = t2.pk;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test t1 USING test t2 WHERE t1.pk = t2.pk;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_ADD",
            intent: "DOLT_ADD is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_BRANCH Force Copy",
            intent: "DOLT_BRANCH Force Copy is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-c', 'main', 'other');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-c', 'main', 'other');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_BRANCH Force Move",
            intent: "DOLT_BRANCH Force Move is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'newother', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-m', 'other', 'newother');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-m', 'other', 'newother');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_BRANCH Delete",
            intent: "DOLT_BRANCH Delete is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'other');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'other');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_CLEAN",
            intent: "DOLT_CLEAN is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_clean();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clean();",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_COMMIT",
            intent: "DOLT_COMMIT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'message');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'message');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_CONFLICTS_RESOLVE",
            intent: "DOLT_CONFLICTS_RESOLVE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', '.');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', '.');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_MERGE",
            intent: "DOLT_MERGE is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'message');",
                "SELECT * FROM dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_RESET",
            intent: "DOLT_RESET is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_REVERT",
            intent: "DOLT_REVERT is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_VERIFY_CONSTRAINTS",
            intent: "DOLT_VERIFY_CONSTRAINTS is refused for a user without a write rule on the branch, and runs once one is added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('-a');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write'), ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('-a');",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_branch_control_other_db_block_tests() {
    run_scripts(&[
        ScriptTest {
            name: "INSERT",
            intent: "INSERT on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REPLACE",
            intent: "REPLACE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/other\".public.test VALUES (2, 2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE",
            intent: "UPDATE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/other\".public.test SET pk = 2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/other\".public.test SET pk = 2;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DELETE",
            intent: "DELETE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/other\".public.test WHERE pk >= 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/other\".public.test WHERE pk >= 0;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TRUNCATE",
            intent: "TRUNCATE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE \"postgres/other\".public.test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE \"postgres/other\".public.test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE AUTO_INCREMENT",
            intent: "ALTER TABLE AUTO_INCREMENT on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE \"postgres/other\".public.test2(pk bigint PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ALTER COLUMN pk RESTART WITH 20;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ALTER COLUMN pk RESTART WITH 20;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD CHECK",
            intent: "ALTER TABLE ADD CHECK on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP CHECK",
            intent: "ALTER TABLE DROP CHECK on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ADD CONSTRAINT check_1 CHECK (pk > 0);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP CONSTRAINT check_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP CONSTRAINT check_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ALTER COLUMN SET DEFAULT",
            intent: "ALTER TABLE ALTER COLUMN SET DEFAULT on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ALTER COLUMN v1 SET DEFAULT (5);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ALTER COLUMN v1 SET DEFAULT (5);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ALTER COLUMN DROP DEFAULT",
            intent: "ALTER TABLE ALTER COLUMN DROP DEFAULT on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ALTER COLUMN v1 SET DEFAULT (5);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD FOREIGN KEY",
            intent: "ALTER TABLE ADD FOREIGN KEY on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                "CREATE TABLE \"postgres/other\".public.test2 (pk bigint PRIMARY KEY, v1 bigint UNIQUE);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ADD CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES \"postgres/other\".public.test (v1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ADD CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES \"postgres/other\".public.test (v1);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP FOREIGN KEY",
            intent: "ALTER TABLE DROP FOREIGN KEY on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                "CREATE TABLE \"postgres/other\".public.test2 (pk bigint PRIMARY KEY, v1 bigint UNIQUE, CONSTRAINT fk_1 FOREIGN KEY (v1) REFERENCES \"postgres/other\".public.test (v1));",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 DROP FOREIGN KEY fk_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 DROP FOREIGN KEY fk_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD INDEX",
            intent: "ALTER TABLE ADD INDEX on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP INDEX",
            intent: "ALTER TABLE DROP INDEX on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP INDEX idx_v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP INDEX idx_v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME INDEX",
            intent: "ALTER TABLE RENAME INDEX on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE \"postgres/other\".public.test ADD INDEX idx_v1 (v1);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER INDEX \"postgres/other\".public.idx_v1 RENAME TO idx_v1_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX \"postgres/other\".public.idx_v1 RENAME TO idx_v1_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD PRIMARY KEY",
            intent: "ALTER TABLE ADD PRIMARY KEY on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE \"postgres/other\".public.test2 (v1 bigint, v2 bigint);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ADD PRIMARY KEY (v1, v2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test2 ADD PRIMARY KEY (v1, v2);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP PRIMARY KEY",
            intent: "ALTER TABLE DROP PRIMARY KEY on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP PRIMARY KEY;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP PRIMARY KEY;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME",
            intent: "ALTER TABLE RENAME on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME TO test_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME TO test_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "RENAME TABLE",
            intent: "RENAME TABLE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME TO test_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME TO test_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD COLUMN",
            intent: "ALTER TABLE ADD COLUMN on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD COLUMN v2 bigint;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test ADD COLUMN v2 bigint;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP COLUMN",
            intent: "ALTER TABLE DROP COLUMN on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP COLUMN v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test DROP COLUMN v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE CHANGE COLUMN",
            intent: "ALTER TABLE CHANGE COLUMN on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test CHANGE COLUMN v1 v1_new bigint;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test CHANGE COLUMN v1 v1_new bigint;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE MODIFY COLUMN",
            intent: "ALTER TABLE MODIFY COLUMN on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test MODIFY COLUMN v1 smallint;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test MODIFY COLUMN v1 smallint;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME COLUMN",
            intent: "ALTER TABLE RENAME COLUMN on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/other\".public.test RENAME COLUMN v1 TO v1_new;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX",
            intent: "CREATE INDEX on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON \"postgres/other\".public.test (v1);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx_v1 ON \"postgres/other\".public.test (v1);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "DROP INDEX",
            intent: "DROP INDEX on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE INDEX idx_v1 ON \"postgres/other\".public.test (v1);",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX idx_v1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE VIEW",
            intent: "CREATE VIEW on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW view_1 AS SELECT * FROM \"postgres/other\".public.test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW view_1 AS SELECT * FROM \"postgres/other\".public.test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "DROP VIEW",
            intent: "DROP VIEW on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE VIEW view_1 AS SELECT * FROM \"postgres/other\".public.test;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP VIEW view_1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW view_1;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE TRIGGER",
            intent: "CREATE TRIGGER on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON \"postgres/other\" FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON \"postgres/other\" FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "DROP TRIGGER",
            intent: "DROP TRIGGER on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "CREATE OR REPLACE FUNCTION \"trigger_1_oracle_trigger\"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN NEW.v1 := 4; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$; CREATE TRIGGER \"trigger_1\" BEFORE INSERT ON \"postgres/other\" FOR EACH ROW EXECUTE FUNCTION \"trigger_1_oracle_trigger\"();",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TRIGGER \"postgres/other\" ON pg_catalog.pg_class;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER \"postgres/other\" ON pg_catalog.pg_class;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE",
            intent: "CREATE TABLE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE LIKE",
            intent: "CREATE TABLE LIKE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 (LIKE \"postgres/other\".public.test INCLUDING ALL);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 (LIKE \"postgres/other\".public.test INCLUDING ALL);",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE AS SELECT",
            intent: "CREATE TABLE AS SELECT on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 AS SELECT * FROM \"postgres/other\".public.test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE \"postgres/other\".public.test2 AS SELECT * FROM \"postgres/other\".public.test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
        ScriptTest {
            name: "DROP TABLE",
            intent: "DROP TABLE on another branch is refused while the user's write rule covers only main, and runs once a rule covers that branch.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'setup commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE \"postgres/other\".public.test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE \"postgres/other\".public.test;",
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("needs DDL through a revision-qualified name such as \"postgres/other\".public.test, which is not built yet"),
            ..S
        },
    ]);
}

#[test]
fn test_branch_control_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Namespace entries block",
            intent: "Namespace rules decide who may create branches whose names they match, the longest match winning.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'other%', 'postgres', 'localhost');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch2');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot create a branch named", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'other%', 'testuser', 'localhost');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'otherbranch%', 'postgres', 'localhost');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch3');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot create a branch named", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'otherbranch%', 'testuser', 'localhost');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Require admin to modify tables",
            intent: "Only a user with admin permission on a branch may change that branch's rules.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER a PASSWORD 'password';",
                "CREATE USER b PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO a; GRANT ALL ON ALL TABLES IN SCHEMA public TO a; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO a;",
                "ALTER ROLE a NOSUPERUSER;",
                "GRANT ALL ON SCHEMA public TO b; GRANT ALL ON ALL TABLES IN SCHEMA public TO b; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO b;",
                "ALTER ROLE b NOSUPERUSER;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'a', 'localhost', 'write'), ('%', 'prefix%', 'a', 'localhost', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE branch = 'other';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the row", message_contains: true, ..E }),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE branch = 'other';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE branch = 'prefix%';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1%', 'b', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE branch = 'prefix1%';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET branch = 'other1' WHERE \"user\" = 'b' AND branch = 'prefix1%';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "to the new branch expression", message_contains: true, ..E }),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET permissions = 'admin' WHERE \"user\" = 'b' AND branch = 'prefix1%';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot update the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET permissions = 'admin' WHERE \"user\" = 'b' AND branch = 'prefix1%';",
                    expected: Expected::Tag("UPDATE 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE branch = 'prefix1%';",
                    expected: Expected::Tag("DELETE 1"),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1%', 'b', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'prefix___', 'a', 'localhost');",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_namespace_control VALUES ('%', 'prefix', 'b', 'localhost');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_namespace_control SET branch = 'prefix%';",
                    expected: Expected::Tag("UPDATE 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_namespace_control SET branch = 'other';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "to the new branch expression", message_contains: true, ..E }),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_namespace_control WHERE branch = 'prefix%';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_namespace_control SET branch = 'anything';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot update the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Deleting entries works",
            intent: "A user with admin permission on matching branches may delete their rules.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE TABLE test (pk bigint PRIMARY KEY);",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost_1', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost_2', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost_3', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost_4', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost_5', 'write');",
                "DELETE FROM dolt_branch_control WHERE host IN ('localhost_2', 'localhost_3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("testuser"), T("localhost_1"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost_4"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost_5"), T("write")],
                        ],
                        tag: "SELECT 4",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE host = 'localhost_5';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("testuser"), T("localhost_1"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost_4"), T("write")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE host = 'localhost_1';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("testuser"), T("localhost"), T("write")],
                            &[T("%"), T("%"), T("testuser"), T("localhost_4"), T("write")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE host = 'localhost_4';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("testuser"), T("localhost"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE \"user\" = 'testuser' AND host = 'localhost';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control;",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'admin');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control;",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("postgres"), T("%"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs as root@localhost, where the scripts run as postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Subset entries count as duplicates",
            intent: "A rule that another rule already covers is refused as a duplicate.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'prefix', 'testuser', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1%', 'testuser', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'prefix2_', 'testuser', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'prefix3_', 'testuser', 'localhost', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1sub%', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "prefix1sub%", "testuser", "localhost", "admin"] already exists."#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1_', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "prefix1_", "testuser", "localhost", "admin"] already exists."#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix2%', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("prefix"), T("testuser"), T("localhost"), T("admin")],
                            &[T("%"), T("prefix1%"), T("testuser"), T("localhost"), T("admin")],
                            &[T("%"), T("prefix2_"), T("testuser"), T("localhost"), T("admin")],
                            &[T("%"), T("prefix2%"), T("testuser"), T("localhost"), T("admin")],
                            &[T("%"), T("prefix3_"), T("testuser"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 5",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "prefix", "testuser", "localhost", "admin"] already exists."#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix1%', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "prefix1%", "testuser", "localhost", "admin"] already exists."#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'prefix3_', 'testuser', 'localhost', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "prefix3_", "testuser", "localhost", "admin"] already exists."#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'postgres';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("postgres"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs as root@localhost, where the scripts run as postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('new_root_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'postgres';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("postgres"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs as root@localhost, where the scripts run as postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rebase respects existing branch control privileges",
            intent: "A user who may write a branch may rebase it.",
            set_up_script: &[
                "CREATE TABLE tbl (i int);",
                "INSERT INTO tbl VALUES (42);",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "SELECT dolt_commit('-A','-m', 'initial data');",
                "SELECT dolt_branch('branch1');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%','%','postgres','%', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%','branch1','testuser','%', 'write');",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser' AND branch = 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("branch1"), T("testuser"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl VALUES (23);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'user1 change');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--interactive', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser' AND branch = 'branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("branch1"), T("testuser"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Creating branch creates new entry",
            intent: "Creating a branch gives its creator admin permission on it.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('otherbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("otherbranch"), T("testuser"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Renaming branch creates new entry",
            intent: "Renaming a branch gives its renamer admin permission on the new name.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "SELECT dolt_branch('otherbranch');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'otherbranch', 'testuser', 'localhost', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("otherbranch"), T("testuser"), T("localhost"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-m', 'otherbranch', 'newbranch');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'otherbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("otherbranch"), T("testuser"), T("localhost"), T("write")],
                            &[T("postgres"), T("newbranch"), T("testuser"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Copying branch creates new entry",
            intent: "Copying a branch gives its copier admin permission on the copy.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "SELECT dolt_branch('otherbranch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-f', '-c', 'otherbranch', 'newbranch');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'otherbranch', 'newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("newbranch"), T("testuser"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Proper database scoping",
            intent: "A rule applies only in the database it names.",
            set_up_script: &[
                "CREATE DATABASE dba;",
                "CREATE DATABASE dbb;",
                "CREATE USER testuser PASSWORD 'password';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                    expected: Expected::Ok,
                    client: "root_dba",
                    database: "dba",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin'), ('dba', 'main', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "root_dba",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                    expected: Expected::Ok,
                    client: "root_dba",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "root_dba",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                    expected: Expected::Ok,
                    client: "root_dbb",
                    database: "dbb",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin'), ('dbb', 'other', 'testuser', 'localhost', 'write');",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "root_dbb",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                    expected: Expected::Ok,
                    client: "root_dbb",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "root_dbb",
                    dolt: Some(Dolt::Differs("Branch control rules apply only in the database that stores them (STORAGE_DESIGN §V.7), and a Postgres connection's database never changes, so each database's rules, grants, and branch are set up through a connection to it, and MySQL's USE dba is a login to dba")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "dba",
                    username: "testuser",
                    password: "password",
                    database: "dba",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE dba is a login to dba, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "dba",
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "dba",
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    client: "dba",
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    client: "dbb",
                    username: "testuser",
                    password: "password",
                    database: "dbb",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE dbb is a login to dbb, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "dbb",
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "dbb",
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("branch control rules are per database (STORAGE_DESIGN §V.7, Appendix C): a rule whose database pattern cannot match the database it is written in is refused, where Dolt keeps one server-wide table"),
            ..S
        },
        ScriptTest {
            name: "Admin privileges do not give implicit branch permissions",
            intent: "A user's database privileges give no permission on branches without a rule.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser WITH GRANT OPTION; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser WITH GRANT OPTION; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser WITH GRANT OPTION;",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'main', 'newbranch');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot delete the branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('newbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("newbranch"), T("testuser"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Database-level admin privileges allow scoped table modifications",
            intent: "Admin permission on a database lets a user add rules for that database only.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE DATABASE dba;",
                "CREATE DATABASE dbb;",
                "CREATE USER a PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO a WITH GRANT OPTION; GRANT ALL ON ALL TABLES IN SCHEMA public TO a WITH GRANT OPTION; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO a WITH GRANT OPTION;",
                "CREATE USER b PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO b WITH GRANT OPTION; GRANT ALL ON ALL TABLES IN SCHEMA public TO b WITH GRANT OPTION; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO b WITH GRANT OPTION;",
                "GRANT ALL ON SCHEMA public TO a; GRANT ALL ON ALL TABLES IN SCHEMA public TO a; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO a;",
                "GRANT ALL ON SCHEMA public TO b; GRANT ALL ON ALL TABLES IN SCHEMA public TO b; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO b;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('dba', 'dummy1', '%', '%', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('db_', 'dummy2', '%', '%', 'write');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('dbb', 'dummy3', '%', '%', 'write');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('dba', 'dummy4', '%', '%', 'write');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('db_', 'dummy5', '%', '%', 'write');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot add the row", message_contains: true, ..E }),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('dbb', 'dummy6', '%', '%', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "b",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE a SUPERUSER;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('db_', 'dummy7', '%', '%', 'write');",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "a",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control;",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("postgres"), T("localhost"), T("admin")],
                            &[T("dba"), T("dummy1"), T("%"), T("%"), T("write")],
                            &[T("dbb"), T("dummy6"), T("%"), T("%"), T("write")],
                            &[T("db_"), T("dummy7"), T("%"), T("%"), T("write")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness runs as root@localhost, where the scripts run as postgres")),
                    ..A
                },
            ],
            skip: Some("branch control rules are per database (STORAGE_DESIGN §V.7, Appendix C): a rule whose database pattern cannot match the database it is written in is refused, where Dolt keeps one server-wide table"),
            ..S
        },
        ScriptTest {
            name: "Cannot insert an exact match even with elevated permissions",
            intent: "A rule that exactly matches an existing one is refused even for an admin.",
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', '%', '%', '%', 'admin');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "%", "%", "%", "admin"] already exists."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE permissions='admin';",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission blocks Write-gated dolt procedures (no SUPER)",
            intent: "Merge permission refuses the Dolt functions that need write permission.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('deadbeef');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--interactive', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_rm('test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission blocks Write-gated dolt procedures (SUPER required)",
            intent: "Merge permission refuses the Dolt functions that need write permission and SUPER, such as backup, fetch, gc, pull, and push.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('add', 'b1', 'file:///tmp/dolt-bc-test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_fetch('origin');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_pull('origin');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', 'main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'r1', 'http://example.com/r1');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_update_column_tag('test', 'v1', 99999);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("dolt_backup's built-in ACL leaves out PUBLIC, so the ACL refuses it (42501) before branch control checks the branch, where Dolt's branch control refuses it"),
            ..S
        },
        ScriptTest {
            name: "Read permission allows read-only dolt procedures",
            intent: "Read permission allows the Dolt functions that only read.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('--from', 'HEAD', '--to', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("ahead", INT8), Column("behind", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission blocks writes to user-space dolt system tables",
            intent: "Merge permission refuses writes to Dolt's user-writable system tables.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('README', '# hello');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_ignore VALUES ('tmp_*', TRUE);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_query_catalog VALUES ('q1', 1, 'count', 'SELECT count(*) FROM test', '');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('t1', NULL, 'SELECT 1', 'expected_single_value', '==', '1');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission blocks writes to per-table artifact system tables",
            intent: "Merge permission refuses writes to the per-table conflict and violation tables outside a merge.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "CREATE TABLE cv (a int, b int); CREATE UNIQUE INDEX  ON cv (a);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO cv VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'cv side');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO cv VALUES (1, 3);",
                "SELECT dolt_commit('-am', 'cv main');",
                "SELECT * FROM dolt_checkout('side');",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT * FROM dolt_merge('main');",
                "INSERT INTO test VALUES (2, 2);",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_test WHERE dolt_id = 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_test SET dolt_staged = TRUE WHERE dolt_id = 0;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_cv;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Read-only dolt system tables reject writes for merge permission",
            intent: "Dolt's read-only system tables refuse writes from a user with merge permission.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branches (name, hash) SELECT 'newbranch', hash FROM dolt_branches WHERE name = 'main';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the dolt_branches table is read-only; use the dolt_branch stored procedure to edit remotes", ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branches WHERE name = 'other';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the dolt_branches table is read-only; use the dolt_branch stored procedure to edit remotes", ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_remotes (name, url, fetch_specs, params) VALUES ('r1', 'http://example.com/r1', '[]', '{}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "the dolt_remotes table is read-only; use the dolt_remote stored procedure to edit remotes", ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("writes to the dolt_branches view fail as Postgres fails writes to a view without rules (55000), not with Dolt's read-only table message"),
            ..S
        },
        ScriptTest {
            name: "Merge permission allows merge but blocks other writes",
            intent: "Merge permission allows a merge into the branch and refuses other writes.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'second commit on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("other"), T("testuser"), T("localhost"), T("merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 3);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = 10 WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk bigint PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clean();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('new_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('v0');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'msg');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission allows non-fast-forward merge (creates merge commit)",
            intent: "Merge permission allows a merge that makes a merge commit.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (3, 3);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on main');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission allows DOLT_CHECKOUT('<branch>') but blocks DOLT_CHECKOUT('<table>')",
            intent: "Merge permission allows checking out a branch and refuses checking out a table.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE test SET v1 = 2 WHERE pk = 1;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Read permission allows DOLT_CHECKOUT('<branch>') but blocks DOLT_CHECKOUT('<table>')",
            intent: "Read permission allows checking out a branch and refuses checking out a table.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE test SET v1 = 2 WHERE pk = 1;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', 'localhost', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission allows merge with conflicts",
            intent: "Merge permission allows a merge that stops with conflicts.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE test SET v1 = 100 WHERE pk = 1;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on other');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET v1 = 200 WHERE pk = 1;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on main');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_test SET our_v1 = 50 WHERE our_pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    username: "testuser",
                    password: "password",
                    skip: Some("writing our_ columns through dolt_conflicts_<t> runs an UPDATE of the table, which branch control checks for write, where Dolt lets merge resolve them"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM test WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    skip: Some("writing our_ columns through dolt_conflicts_<t> runs an UPDATE of the table, which branch control checks for write, where Dolt lets merge resolve them"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolved');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v1 = 100 WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission allows DELETE from dolt_conflicts_<t> to resolve conflicts",
            intent: "Merge permission allows resolving a merge's conflicts by deleting them.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE test SET v1 = 100 WHERE pk = 1;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on other');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET v1 = 200 WHERE pk = 1;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'commit on main');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Tag("DELETE 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v1 FROM test WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission does not allow conflicts writes outside an active merge",
            intent: "Merge permission refuses writes to conflict tables when no merge is in progress.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 'test');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Read-only user cannot merge",
            intent: "Read permission refuses merging.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'second commit on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 3);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge permission on specific branch does not apply to other branches",
            intent: "Merge permission on one branch gives none on others.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "ALTER ROLE testuser NOSUPERUSER;",
                "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                "INSERT INTO test VALUES (1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_branch('third');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'other', 'testuser', 'localhost', 'merge');",
                "INSERT INTO test VALUES (2, 2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'second commit on main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('third');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'third'")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Mixed any match operator (single, prefix, postfix)",
            intent: "Rules match branch names with single-character, prefix, and suffix patterns.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%wy', 'prefix%', '%', '%', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%wy', 'mask%', '%', '%', 'write');",
                "CREATE TABLE test (pk bigint);",
                "CREATE USER testuser PASSWORD 'password';",
                "GRANT USAGE ON SCHEMA public TO testuser; GRANT SELECT, INSERT ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT ON TABLES TO testuser;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (6);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "does not have the correct permissions on branch", message_contains: true, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            skip: Some("branch control rules are per database (STORAGE_DESIGN §V.7, Appendix C): a rule whose database pattern cannot match the database it is written in is refused, where Dolt keeps one server-wide table"),
            ..S
        },
        ScriptTest {
            name: "Allow restrictions and supersets",
            intent: "A rule that an existing one covers is refused, while a restriction or a superset of it may be added.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', '%', '%', 'write');",
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE USER testuser2 PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO testuser; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser;",
                "GRANT ALL ON SCHEMA public TO testuser2; GRANT ALL ON ALL TABLES IN SCHEMA public TO testuser2; ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO testuser2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', '%', 'write');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "main", "testuser", "%", "write"] already exists."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', '%', 'read');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser2', '%', 'admin');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_branch_control WHERE \"user\" = 'testuser';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET \"user\" = 'testuser', permissions = 'read' WHERE \"user\" = 'testuser2';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET \"user\" = 'testuser2', permissions = 'admin' WHERE \"user\" = 'testuser';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_branch_control SET permissions = 'write' WHERE \"user\" = 'testuser2';",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint", message_contains: true, detail: r#"Key ["%", "main", "testuser2", "%", "write"] already exists."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_branch_control_postgres_tests() {
    run_scripts(&[
        ScriptTest {
            name: "A transaction is checked against its own uncommitted rules",
            intent: "A rule edit is visible to the rest of its own transaction, and ROLLBACK restores the rules before it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_branch_control SET permissions = 'merge' WHERE "user" = 'postgres';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SET ROLE checks the role set, and RESET ROLE the session user",
            intent: "Branch control checks the current user, as Postgres' privilege checks do, where Dolt checks the connection's user.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET ROLE testuser;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RESET ROLE;"#,
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A SECURITY DEFINER function writes with its owner's rules",
            intent: "Inside a SECURITY DEFINER function the current user is the function's owner, whose rules apply.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "CREATE FUNCTION add_row(v int) RETURNS void LANGUAGE sql SECURITY DEFINER AS 'INSERT INTO t VALUES (v)';",
                "GRANT EXECUTE ON FUNCTION add_row(int) TO testuser;",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT add_row(2);"#,
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A team role's rule lets its members write",
            intent: "A rule naming a role applies to every role with its privileges, as Postgres grants do, where Dolt matches only the user's own name.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "CREATE ROLE team;",
                "GRANT team TO testuser;",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('release/1.0');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'release/%', 'team', '%', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO "postgres/release/1.0".public.t VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A member's own narrower rule doesn't take away the team's write",
            intent: "Rules of the roles a user has the privileges of add up, as Postgres grants never deny.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "CREATE ROLE team;",
                "GRANT team TO testuser;",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('release/1.0');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'release/%', 'team', '%', 'write');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'release/1.0', 'testuser', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO "postgres/release/1.0".public.t VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A NOINHERIT member needs SET ROLE for the team's rule",
            intent: "A membership without INHERIT gives no privileges until SET ROLE, so the team's rules apply only after it.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "ALTER ROLE testuser NOINHERIT;",
                "CREATE ROLE team;",
                "GRANT team TO testuser;",
                "GRANT ALL ON t TO team;",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('release/1.0');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', 'localhost', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'release/%', 'team', '%', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO "postgres/release/1.0".public.t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `release/1.0`"#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET ROLE team;"#,
                    expected: Expected::Tag("SET"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO "postgres/release/1.0".public.t VALUES (2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A superuser goes through branch control, and edits the rules to write",
            intent: "As in Dolt, a superuser's writes are checked like anyone's, and its one extra power is editing any rule.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'postgres', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_branch_control SET permissions = 'write' WHERE "user" = 'postgres';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A rule must be able to match its database's name",
            intent: "Each database keeps its own rules, so a rule whose database pattern cannot match the database is refused, where Dolt keeps one server-wide table.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('other_db', '%', 'u', '%', 'write');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch control rules of database "postgres" must match its name"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('post%', 'b', 'u', '%', 'admin');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A merge bringing one rule with other permissions conflicts on its row",
            intent: "Rules live in the database tree, so a pull whose rule has the same patterns as a local one with other permissions is a conflict on that row (STORAGE_DESIGN §V.2).",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A rule naming a role follows its renames and keeps its name once it is dropped",
            intent: "A rule written with a role's literal name keeps the role, so ALTER ROLE RENAME carries it along, where Dolt's rule keeps the old name; DROP ROLE leaves the name as text, so the rule matches that name as Dolt's would.",
            set_up_script: &[
                "CREATE ROLE bob;",
                "INSERT INTO dolt_branch_control VALUES ('%', 'b%', 'bob', '%', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER ROLE bob RENAME TO robert;"#,
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "user" FROM dolt_branch_control WHERE branch = 'b%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("user", TEXT)],
                        rows: &[
                            &[T("robert")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP ROLE robert;"#,
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "user" FROM dolt_branch_control WHERE branch = 'b%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("user", TEXT)],
                        rows: &[
                            &[T("robert")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A function's DDL is checked like the same statement sent alone",
            intent: "Statements a function runs go through branch control as statements a client sends do.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN CREATE TABLE t (pk int PRIMARY KEY); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_class WHERE relname = 't';"#,
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
        ScriptTest {
            name: "Statements that change nothing a branch keeps need no write",
            intent: "Roles, databases, settings' privileges, and temporary relations live outside the branch, so a user who may only read it still changes them.",
            set_up_script: &[
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE ROLE r;"#,
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER ROLE r RENAME TO r2;"#,
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON ROLE r2 IS 'a role';"#,
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT CREATE ON DATABASE postgres TO r2;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON DATABASE postgres IS 'a database';"#,
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SET ON PARAMETER work_mem TO r2;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REVOKE SET ON PARAMETER work_mem FROM r2;"#,
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REVOKE CREATE ON DATABASE postgres FROM r2;"#,
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP ROLE r2;"#,
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TEMP TABLE tt (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE pg_temp.tq (pk int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX ON tt (pk);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE tt ADD COLUMN v int;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE tt RENAME TO tt2;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TEMP VIEW tv AS SELECT 1 AS a;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TEMP SEQUENCE ts;"#,
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER SEQUENCE ts RESTART WITH 10;"#,
                    expected: Expected::Tag("ALTER SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT nextval('ts'), setval('ts', 20);"#,
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8), Column("setval", INT8)],
                        rows: &[
                            &[T("10"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TEMP TABLE ta AS SELECT 1 AS a;"#,
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1 AS a INTO TEMP tb;"#,
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE tt2, tq, ta, tb;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP VIEW tv;"#,
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP SEQUENCE ts;"#,
                    expected: Expected::Tag("DROP SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TEMP TABLE t (pk int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE public.t (pk int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p (pk int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE SEQUENCE s;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW v AS SELECT * FROM t;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    skip: Some("a view over a temporary table becomes temporary only once it is defined, after branch control has checked it as a view of the branch"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Sequences and large objects of the branch need write",
            intent: "nextval, setval, and the large object functions change the branch, as the read-only check refuses them in a read-only transaction.",
            set_up_script: &[
                "CREATE SEQUENCE s;",
                "SELECT lo_from_bytea(4242, 'x');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT nextval('s');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT setval('s', 5);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT lo_create(0);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT lo_from_bytea(0, 'y');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT lo_put(4242, 0, 'z');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT lo_unlink(4242);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT encode(lo_get(4242), 'escape');"#,
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Making an existing branch again needs the right to delete it",
            intent: "dolt_branch -f replaces the branch it names, so it needs the permission deleting that branch does, where Dolt checks only that the branch may be created.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "SELECT dolt_commit('--allow-empty', '-m', 'setup');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'b1', 'testuser', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-f', 'b1', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` cannot delete the branch `b1`"#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-f', 'b2', 'main');"#,
                    expected: Expected::Ok,
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Statements on a temporary table need no write",
            intent: "A user who may only read the branch uses their own temporary tables as fully as their rows, while the same statements on the branch's tables are refused, except CLUSTER and REINDEX, which a read-only transaction allows too.",
            set_up_script: &[
                "CREATE TABLE p (pk int PRIMARY KEY);",
                "CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TEMP TABLE tt (pk int PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON TABLE tt IS 'a temporary table';"#,
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON COLUMN tt.v IS 'a column';"#,
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE tt;"#,
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CLUSTER tt USING tt_pkey;"#,
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REINDEX TABLE tt;"#,
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REINDEX INDEX tt_pkey;"#,
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER tg BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION f();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TRIGGER tg ON tt;"#,
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE RULE r AS ON INSERT TO tt DO INSTEAD NOTHING;"#,
                    expected: Expected::Tag("CREATE RULE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP RULE r ON tt;"#,
                    expected: Expected::Tag("DROP RULE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE p;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON TABLE p IS 'a table';"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CLUSTER p USING p_pkey;"#,
                    expected: Expected::Tag("CLUSTER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REINDEX TABLE p;"#,
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER tg BEFORE INSERT ON p FOR EACH ROW EXECUTE FUNCTION f();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE RULE r AS ON INSERT TO p DO INSTEAD NOTHING;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A commit on another branch checks that branch's rules",
            intent: "dolt_commit --branch commits another branch's working set, so it needs merge on that branch, not on the session's.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_checkout('other');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_checkout('main');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', '%', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--branch', 'other', '-Am', 'other');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `other`"#, ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A sequence of another branch checks that branch's rules",
            intent: "nextval on a sequence named through another branch advances that branch's sequence, so it needs write on that branch.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE SEQUENCE s;",
                "GRANT USAGE ON SEQUENCE s TO testuser;",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('other');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', 'main', 'testuser', '%', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT nextval('s');"#,
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "testuser",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT nextval('"postgres/other".public.s');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`testuser`@`localhost` does not have the correct permissions on branch `other`"#, ..E }),
                    skip: Some("regclass input does not take a revision-qualified name yet"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Database names compare exactly, and branch names and hosts ignoring ASCII case",
            intent: "Rules compare names as Postgres and Doltgrust do, database names as case-sensitive identifiers and branch names and hosts ignoring ASCII case, where Dolt compares database, branch, and host patterns under MySQL's utf8mb4_0900_ai_ci.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT ALL ON t TO testuser;",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'admin');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('POSTGRES', '%', 'testuser', '%', 'write');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch control rules of database "postgres" must match its name"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('postgres', 'MAIN', 'testuser', 'LOCALHOST', 'write');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT branch, host FROM dolt_branch_control WHERE "user" = 'testuser';"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("host", TEXT)],
                        rows: &[
                            &[T("main"), T("localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Deleting a remote-tracking branch checks the name as given",
            intent: "Dolt's deleteBranches checks every name dolt_branch -d deletes, remote-tracking ones included, against the rules before it deletes any.",
            set_up_script: &[
                "CREATE USER testuser PASSWORD 'password';",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'postgres', '%', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'testuser', '%', 'read');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-d', '-r', 'origin/main');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: "`testuser`@`localhost` cannot delete the branch `origin/main`", ..E }),
                    username: "testuser",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
