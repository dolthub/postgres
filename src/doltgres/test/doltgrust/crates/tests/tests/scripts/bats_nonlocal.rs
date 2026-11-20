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

//! Dolt's integration-tests/bats/nonlocal.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_nonlocal() {
    run_scripts(&[
        ScriptTest {
            name: "nonlocal: branch name reflects the working set of the referenced branch",
            intent: "A rule naming a branch reads the table as that branch's working set has it, uncommitted rows included.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('table_alias_branch', 'main', 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_branch;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: branch ref reflects the committed version of the parent",
            intent: "A rule naming heads/<branch> reads the branch's head commit, leaving out its uncommitted rows.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT dolt_commit('-Am', 'create table');",
                "INSERT INTO aliased_table VALUES ('eesekkgo');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('table_alias_branch_ref', 'heads/main', 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_branch_ref;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: tag and hash",
            intent: "Rules naming a tag, tags/<tag>, or a commit hash read that commit's table, which refuses writes.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT dolt_commit('-Am', 'commit');",
                "SELECT dolt_tag('v1.0');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('table_alias_tag', 'v1.0', 'aliased_table', 'immediate'), ('table_alias_tag_ref', 'tags/v1.0', 'aliased_table', 'immediate'), ('table_alias_hash', dolt_hashof('v1.0'), 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_tag_ref;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_tag;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_hash;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table_alias_tag_ref VALUES ('eesekkgo');",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt says \"table doesn't support INSERT INTO\"; a write to a revision fails as Postgres fails one in a read-only transaction, as AS OF writes do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table_alias_tag VALUES ('eesekkgo');",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt says \"table doesn't support INSERT INTO\"; a write to a revision fails as Postgres fails one in a read-only transaction, as AS OF writes do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table_alias_hash VALUES ('eesekkgo');",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt says \"table doesn't support INSERT INTO\"; a write to a revision fails as Postgres fails one in a read-only transaction, as AS OF writes do")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: remote ref",
            intent: "Rules naming a remote-tracking branch, as <remote>/<branch> or remotes/<remote>/<branch>, read its commit's table.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT dolt_remote('add', 'remote_db', 'file://./remote');",
                "SELECT * FROM dolt_push('remote_db', 'main');",
                "DROP TABLE aliased_table;",
                "SELECT dolt_commit('-am', 'drop table');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('table_alias_remote_branch', 'remote_db/main', 'aliased_table', 'immediate'), ('table_alias_remote_ref', 'remotes/remote_db/main', 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_remote_branch;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias_remote_ref;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: default ref",
            intent: "A rule without a target reads the session's own branch, so one name aliases another table of the branch.",
            set_up_script: &[
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "INSERT INTO dolt_nonlocal_tables(table_name, ref_table, options) VALUES ('table_alias', 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: default table_name",
            intent: "A rule without ref_table reads the target's table of the same name.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE table_alias (pk char(8) PRIMARY KEY);",
                "INSERT INTO table_alias VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('table_alias', 'main', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM table_alias;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: wildcard table_name",
            intent: "A wildcard rule covers each target table whose name it matches, as dolt_ignore's patterns match, and no other.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('nonlocal_*', 'main', 'immediate');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE nonlocal_table1 (pk char(8) PRIMARY KEY);",
                "CREATE TABLE nonlocal_table2 (pk char(8) PRIMARY KEY);",
                "CREATE TABLE not_nonlocal_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO nonlocal_table1 VALUES ('amzmapqt');",
                "INSERT INTO nonlocal_table2 VALUES ('eesekkgo');",
                "INSERT INTO not_nonlocal_table VALUES ('pzdxwmbd');",
                "SELECT * FROM dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM not_nonlocal_table;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"not_nonlocal_table\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: a transaction commits inserts to local and nonlocal branches",
            intent: "One transaction's inserts into a local table and a nonlocal one commit to both branches' working sets.",
            set_up_script: &[
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO local_table VALUES ('amzmapqt');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nonlocal_table VALUES ('eesekkgo');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM local_table;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.aliased_table;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: a transaction commits updates and deletes to local and nonlocal branches",
            intent: "One transaction's updates and deletes of a local table and a nonlocal one commit to both branches, and the alias leaves the branch's own table of the target's name alone.",
            set_up_script: &[
                "CREATE TABLE aliased_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO aliased_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'set up main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE local_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO local_table VALUES (1, 100), (2, 200);",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'set up other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE local_table SET v = v + 1 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE nonlocal_table SET v = v + 5 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM local_table WHERE pk = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM nonlocal_table WHERE pk = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM local_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.aliased_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM aliased_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: savepoint rollback preserves earlier changes on both branches",
            intent: "Rolling back to a savepoint undoes the later changes on both branches and keeps the earlier ones.",
            set_up_script: &[
                "CREATE TABLE aliased_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO aliased_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'set up main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE local_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO local_table VALUES (1, 100), (2, 200);",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'set up other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE local_table SET v = 101 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE nonlocal_table SET v = 11 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT both_branches;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM local_table WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM nonlocal_table WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO local_table VALUES (3, 300);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nonlocal_table VALUES (3, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT both_branches;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM local_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                            &[T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.aliased_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: dolt_commit_all commits local and nonlocal branch heads",
            intent: "dolt_commit_all commits every branch the transaction changed, the nonlocal table's target included.",
            set_up_script: &[
                "CREATE TABLE aliased_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO aliased_table VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'set up main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE local_table (pk int PRIMARY KEY, v int);",
                "INSERT INTO local_table VALUES (1, 100), (2, 200);",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'set up other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE local_table SET v = 101 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE nonlocal_table SET v = 11 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit_all('-am', 'update both branches');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit_all", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM local_table AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                            &[T("2"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM aliased_table AS OF 'main' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update both branches")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) AS dirty_tables FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("dirty_tables", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_commit_all returns a (branch, hash) record for each head it commits, where the port expects one text value"),
            ..S
        },
        ScriptTest {
            name: "nonlocal: test foreign keys",
            intent: "A foreign key may reference a nonlocal table, and dolt_verify_constraints reports the violation once the target's rows go.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "INSERT INTO nonlocal_table VALUES ('eesekkgo');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
                "INSERT INTO local_table VALUES ('eesekkgo');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO local_table VALUES ('fdnfjfjf');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"local_table\" violates foreign key constraint \"local_table_pk_fkey\"", detail: "Key (pk)=(fdnfjfjf) is not present in table \"nonlocal_table\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/main\".public.aliased_table;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT violation_type, pk FROM dolt_constraint_violations_local_table;",
                    expected: Expected::Rows {
                        columns: &[Column("violation_type", TEXT), Column("pk", BPCHAR)],
                        rows: &[
                            &[T("foreign key"), T("eesekkgo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("foreign keys referencing nonlocal tables are refused until a stored constraint can name another branch's table (Appendix C)"),
            ..S
        },
        ScriptTest {
            name: "nonlocal: detect foreign key violations when rows are deleted from the nonlocal table",
            intent: "Dropping a nonlocal table's target leaves a foreign key dolt_verify_constraints can't check, and the foreign key stays.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "INSERT INTO nonlocal_table VALUES ('eesekkgo');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
                "INSERT INTO local_table VALUES ('eesekkgo');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE \"postgres/main\".public.aliased_table;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nonlocal_table\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_constraint WHERE conname = 'local_table_pk_fkey';",
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
            skip: Some("foreign keys referencing nonlocal tables are refused until a stored constraint can name another branch's table (Appendix C)"),
            ..S
        },
        ScriptTest {
            name: "nonlocal: trying to dolt_add a nonlocal table returns the appropriate warning",
            intent: "dolt_add of a name a rule gives another branch's table fails as for a table that doesn't exist.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE nonlocal_table (pk char(8) PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('nonlocal_table', 'main', 'immediate');",
                "INSERT INTO nonlocal_table VALUES ('ghdsgerg');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('nonlocal_table');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: the table(s) nonlocal_table do not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: dolt_add('.') doesn't add nonlocal tables",
            intent: "dolt_add('.') stages the branch's own changes and leaves the target branch's table unstaged there.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE test_table (pk char(8) PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('test_table', 'main', 'immediate');",
                "INSERT INTO test_table VALUES ('ghdsgerg');",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_nonlocal_tables"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test_table"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: self-referrential nonlocal tables in the same branch as their target are effectively ignored",
            intent: "A rule naming the session's own branch and table leaves the table local, so staging it works.",
            set_up_script: &[
                "CREATE TABLE nonlocal_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('nonlocal_table', 'main', 'immediate');",
                "INSERT INTO nonlocal_table VALUES ('ghdsgerg');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('nonlocal_table');",
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
                    query: "SELECT table_name, staged FROM dolt_status WHERE table_name = 'public.nonlocal_table';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL)],
                        rows: &[
                            &[T("public.nonlocal_table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT table_name, staged FROM dolt_status WHERE table_name = 'public.nonlocal_table';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL)],
                        rows: &[
                            &[T("public.nonlocal_table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal: adding an existing table to nonlocal tables errors",
            intent: "Adding a rule for a name the branch already has a table of fails.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "CREATE TABLE nonlocal_table (pk char(8) PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES ('nonlocal_table', 'main', 'immediate');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot make nonlocal table nonlocal_table, table already exists on branch other", ..E }),
                    ..A
                },
            ],
            skip: Some("Dolt skips it too: Dolt doesn't refuse such a rule"),
            ..S
        },
    ]);
}
