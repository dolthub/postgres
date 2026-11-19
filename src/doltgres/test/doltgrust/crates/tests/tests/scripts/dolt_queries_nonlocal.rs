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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_nonlocal.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_nonlocal_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "basic nonlocal tables use case",
            intent: "A dolt_nonlocal_tables rule makes a table name on one branch read and write another branch's table, which foreign keys can reference.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES\n				('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "INSERT INTO nonlocal_table VALUES ('eesekkgo');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.aliased_table;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'nonlocal_table' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'local_table' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO local_table VALUES ('amzmapqt');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
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
            ],
            skip: Some("foreign keys referencing nonlocal tables are refused until a stored constraint can name another branch's table (Appendix C)"),
            ..S
        },
        ScriptTest {
            name: "detect foreign key invalidation is detected when rows are removed",
            intent: "Removing referenced rows from a nonlocal table's target leaves foreign key violations that dolt_verify_constraints reports.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES\n				('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
                "INSERT INTO local_table VALUES ('amzmapqt');",
                "DELETE FROM \"postgres/main\".public.aliased_table;",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
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
                    query: "SELECT violation_type FROM dolt_constraint_violations_local_table;",
                    expected: Expected::Rows {
                        columns: &[Column("violation_type", TEXT)],
                        rows: &[
                            &[T("foreign key")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'local_table' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            skip: Some("foreign keys referencing nonlocal tables are refused until a stored constraint can name another branch's table (Appendix C)"),
            ..S
        },
        ScriptTest {
            name: "detect foreign key invalidation is detected when the nonlocal table is dropped",
            intent: "Dropping a nonlocal table's target leaves foreign key violations that dolt_verify_constraints reports.",
            set_up_script: &[
                "CREATE DATABASE IF NOT EXISTS postgres;",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES\n				('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
                "INSERT INTO local_table VALUES ('amzmapqt');",
                "DROP TABLE \"postgres/main\".public.aliased_table;",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
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
                    query: "SELECT violation_type FROM dolt_constraint_violations_local_table;",
                    expected: Expected::Rows {
                        columns: &[Column("violation_type", TEXT)],
                        rows: &[
                            &[T("foreign key")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'local_table' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            skip: Some("Dolt main fails it too; foreign keys referencing nonlocal tables are refused until a stored constraint can name another branch's table (Appendix C)"),
            ..S
        },
        ScriptTest {
            name: "creating a table matching a nonlocal table rule results in an error",
            intent: "Creating a table whose name a nonlocal rule covers fails on other branches.",
            set_up_script: &[
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES\n				('nonlocal_table', 'main', 'immediate');",
                "SELECT dolt_commit('-Am', 'add nonlocal rule');",
                "SELECT dolt_branch('other');",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE nonlocal_table (pk char(8) PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name nonlocal_table because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating a table matching a nonlocal table rule is only allowed on the rule's target branch",
            intent: "A table a nonlocal rule covers can be created only on the rule's target branch.",
            set_up_script: &[
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES\n				('global_*', 'main', 'immediate');",
                "SELECT dolt_commit('-Am', 'add nonlocal rule');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE global_a (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE global_b (id int, CONSTRAINT pk_b PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE global_c (id int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name global_c because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE global_d (id int, CONSTRAINT pk_d PRIMARY KEY (id));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name global_d because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE TABLE IF NOT EXISTS for existing nonlocal table succeeds on non-target branch",
            intent: "CREATE TABLE IF NOT EXISTS of an existing nonlocal table succeeds on another branch.",
            set_up_script: &[
                "CREATE TABLE global_migrations_history (migration_id varchar(150) NOT NULL, product_version varchar(32) NOT NULL, CONSTRAINT pk_global_migrations_history PRIMARY KEY (migration_id));",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES ('global_*', 'main', 'immediate');",
                "INSERT INTO dolt_ignore (pattern, ignored) VALUES ('global_*', TRUE);",
                "SELECT dolt_commit('-Am', 'share global_* tables across branches');",
                "INSERT INTO global_migrations_history VALUES ('20240101000000_Initial', '10.0.0');",
                "SELECT dolt_branch('schema');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS global_migrations_history (migration_id varchar(150) NOT NULL, product_version varchar(32) NOT NULL, CONSTRAINT pk_global_migrations_history PRIMARY KEY (migration_id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "42P07", message: "relation \"global_migrations_history\" already exists, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('schema');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'schema'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM global_migrations_history;",
                    expected: Expected::Rows {
                        columns: &[Column("migration_id", VARCHAR), Column("product_version", VARCHAR)],
                        rows: &[
                            &[T("20240101000000_Initial"), T("10.0.0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'global_migrations_history';",
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
                    query: "CREATE TABLE IF NOT EXISTS global_migrations_history (migration_id varchar(150) NOT NULL, product_version varchar(32) NOT NULL, CONSTRAINT pk_global_migrations_history PRIMARY KEY (migration_id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "42P07", message: "relation \"global_migrations_history\" already exists, skipping", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "creating a table matching a nonlocal table rule with ref_table set or a frozen tag target is rejected on the target branch",
            intent: "A nonlocal rule with ref_table or a tag target refuses creating the covered table even on its target.",
            set_up_script: &[
                "SELECT dolt_tag('v1');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES\n				('renamed_*', 'main', 'other_name', 'immediate'),\n				('tagged_*', 'v1', '', 'immediate');",
                "SELECT dolt_commit('-Am', 'add nonlocal rules');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE renamed_a (id int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name renamed_a because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tagged_a (id int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name tagged_a because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables appear in 'show tables'",
            intent: "Nonlocal tables are listed with the branch's tables.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "CREATE TABLE table_alias_1 (pk char(8) PRIMARY KEY);",
                "CREATE TABLE table_alias_wild_3 (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES\n				('table_alias_1', 'main', '', 'immediate'),\n				('table_alias_2', 'main', 'aliased_table', 'immediate'),\n				('table_alias_wild_*', 'main', '', 'immediate'),\n				('table_alias_missing', 'main', '', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("table_alias_1")],
                            &[T("table_alias_2")],
                            &[T("table_alias_wild_3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "detect invalid options",
            intent: "dolt_nonlocal_tables refuses invalid options.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, options) VALUES\n				('nonlocal_table', 'main', 'invalid');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid nonlocal table options invalid: only valid value is 'immediate'.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal table appears once in show tables when local table exists",
            intent: "A nonlocal table listed alongside a local table of the same name appears once.",
            set_up_script: &[
                "CREATE TABLE foo (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create table foo');",
                "SELECT dolt_branch('test');",
                "INSERT INTO foo VALUES (1);",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES ('foo', 'main', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema override permits ignored nonlocal tables",
            intent: "A schema override reads tables a nonlocal rule ignores.",
            set_up_script: &[
                "SELECT dolt_tag('empty_schema');",
                "CREATE TABLE global_test (id int PRIMARY KEY, name varchar(100)); CREATE INDEX  ON global_test (name);",
                "INSERT INTO global_test VALUES (1, 'one'), (2, NULL);",
                "CREATE TABLE global_empty (id int PRIMARY KEY);",
                "CREATE TABLE local_only (id int PRIMARY KEY);",
                "INSERT INTO dolt_ignore VALUES ('global_*', TRUE), ('local_only', TRUE);",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES ('global_*', 'main', 'immediate');",
                "SELECT dolt_commit('-Am', 'configure nonlocal tables');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'main';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM global_test;",
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
                    query: "SELECT * FROM global_test ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM global_test WHERE name = 'one';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM global_empty;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM local_only;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "unable to find table \"local_only\" at overridden schema root", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'empty_schema';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM global_test;",
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
                    query: "SET dolt_override_schema = 'doesNotExist';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM global_test;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to resolve schema override value: branch not found: doesNotExist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema override permits ignored nonlocal aliases on another branch",
            intent: "A schema override reads nonlocal aliases a rule ignores on another branch.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('global_*', TRUE);",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, ref_table, options) VALUES ('alias', 'main', 'global_test', 'immediate');",
                "SELECT dolt_commit('-Am', 'configure nonlocal alias');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE global_test (id int PRIMARY KEY, name varchar(100));",
                "INSERT INTO global_test VALUES (1, 'one');",
                "SELECT * FROM dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'main';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM alias;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.global_test;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "unable to find table \"global_test\" at overridden schema root", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema override maps committed nonlocal tables",
            intent: "A schema override maps committed nonlocal tables to their targets.",
            set_up_script: &[
                "SELECT dolt_tag('empty_schema');",
                "CREATE TABLE target (id int PRIMARY KEY, name varchar(100));",
                "INSERT INTO target VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'create target');",
                "SELECT dolt_tag('original');",
                "ALTER TABLE target ADD COLUMN extra int;",
                "INSERT INTO target VALUES (2, 'two', 20);",
                "SELECT dolt_commit('-am', 'extend target');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, ref_table, options) VALUES ('alias', 'main', 'target', 'immediate'), ('tag_alias', 'original', 'target', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'original';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM alias ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tag_alias;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'empty_schema';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tag_alias;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM alias ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("extra", INT4)],
                        rows: &[
                            &[T("1"), T("one"), Null],
                            &[T("2"), T("two"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict updates use the local backing table despite a nonlocal rule",
            intent: "Conflict resolution updates the local table even when a nonlocal rule covers its name.",
            set_up_script: &[
                "CREATE TABLE target (id int PRIMARY KEY, value int);",
                "INSERT INTO target VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'create target');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE target SET value = 10;",
                "SELECT dolt_commit('-am', 'other value');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE target SET value = 20;",
                "SELECT dolt_commit('-am', 'main value');",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT * FROM dolt_merge('other');",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES ('target', 'other', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT base_value, our_value, their_value FROM dolt_conflicts_target;",
                    expected: Expected::Rows {
                        columns: &[Column("base_value", INT4), Column("our_value", INT4), Column("their_value", INT4)],
                        rows: &[
                            &[T("0"), T("20"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT value FROM target;",
                    expected: Expected::Rows {
                        columns: &[Column("value", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_conflicts_target SET our_value = 30 WHERE our_id = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_nonlocal_tables WHERE table_name = 'target';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT value FROM target;",
                    expected: Expected::Rows {
                        columns: &[Column("value", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT value FROM \"postgres/other\".public.target;",
                    expected: Expected::Rows {
                        columns: &[Column("value", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal table is not affected by dolt_clean()",
            intent: "dolt_clean leaves nonlocal tables alone.",
            set_up_script: &[
                "CREATE TABLE global_test (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, name varchar(100));",
                "INSERT INTO global_test (id, name) VALUES (1, 'one');",
                "CREATE TABLE foo (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY);",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, options) VALUES ('global_*', 'main', 'immediate');",
                "SELECT dolt_add('dolt_nonlocal_tables');",
                "SELECT dolt_commit('-m', 'set up dolt_nonlocal_tables');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM global_test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clean();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM global_test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("global_test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clean('-x');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM global_test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("global_test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables read and write another branch's table, and a foreign key to one is refused",
            intent: "The basic use case without the foreign key Doltgrust refuses: the name reads and writes the target branch's working set, and the session's branch stays clean.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk char(8) PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables(table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'add the rule');",
                "INSERT INTO nonlocal_table VALUES ('eesekkgo');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM nonlocal_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.aliased_table ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", BPCHAR)],
                        rows: &[
                            &[T("amzmapqt")],
                            &[T("eesekkgo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status;",
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
                    query: "CREATE TABLE local_table (pk char(8) PRIMARY KEY, FOREIGN KEY (pk) REFERENCES nonlocal_table(pk));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "foreign keys referencing nonlocal table \"nonlocal_table\" are not supported", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables write through the target branch's constraints and triggers",
            intent: "An INSERT through a name a rule gives a table of another branch runs that branch's triggers and checks its constraints.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int CHECK (v > 0));",
                "CREATE FUNCTION tenfold() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.v := NEW.v * 10; RETURN NEW; END $$;",
                "CREATE TRIGGER t_tenfold BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION tenfold();",
                "SELECT dolt_commit('-Am', 'add t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "DROP TRIGGER t_tenfold ON t;",
                "ALTER TABLE t DROP CONSTRAINT t_v_check;",
                "INSERT INTO dolt_nonlocal_tables VALUES ('t', 'main', '', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, -1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"t_v_check\"", detail: "Failing row contains (2, -10).", schema: "public", table: "t", constraint: "t_v_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_nonlocal_tables;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, -1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables of a committed revision are read only, aliases of the session's branch read the table itself",
            intent: "A tag target reads the tag's table and refuses writes, an alias of the session's own branch reads and writes its table, and a wildcard rule with a table covers each name it matches.",
            set_up_script: &[
                "CREATE TABLE real_t (pk int PRIMARY KEY);",
                "INSERT INTO real_t VALUES (1);",
                "CREATE TABLE other_name (pk int PRIMARY KEY);",
                "INSERT INTO other_name VALUES (7);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_tag('v1');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO dolt_nonlocal_tables VALUES ('alias', '', 'real_t', 'immediate'), ('renamed_*', 'main', 'other_name', 'immediate'), ('tagged', 'v1', 'real_t', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM tagged;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tagged VALUES (9);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    dolt: Some(Dolt::Differs("Dolt says \"table doesn't support INSERT INTO\"; Doltgrust refuses a write to a revision as Postgres refuses one in a read-only transaction")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO alias VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM real_t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM renamed_x;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE renamed_y (pk int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"renamed_y\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("other_name")],
                            &[T("real_t")],
                            &[T("tagged")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt lists the alias and the raw pattern renamed_*; the alias is listed once under its table's own name and a wildcard rule's names aren't listed (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables whose target branch is deleted fail to read",
            intent: "Once a rule's target branch is deleted, reading or creating a name it covers fails as Dolt's does, while the catalogs stay readable.",
            set_up_script: &[
                "SELECT dolt_branch('tgt');",
                "SELECT * FROM dolt_checkout('tgt');",
                "CREATE TABLE x (pk int PRIMARY KEY);",
                "INSERT INTO x VALUES (1);",
                "SELECT dolt_commit('-Am', 'x on tgt');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO dolt_nonlocal_tables VALUES ('x', 'tgt', '', 'immediate');",
                "SELECT dolt_commit('-Am', 'the rule');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM x;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'tgt');",
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
                    query: "SELECT * FROM x;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: tgt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x (pk int);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: tgt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's SHOW TABLES fails too once the target is gone (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables follow rules and targets per statement in READ COMMITTED and the snapshot in REPEATABLE READ",
            intent: "Another session's change of the rules or of a target branch's rows is seen by the next statement of a READ COMMITTED transaction, and a REPEATABLE READ transaction keeps its snapshot's rules and rows.",
            set_up_script: &[
                "CREATE TABLE shared (pk int PRIMARY KEY);",
                "INSERT INTO shared VALUES (1);",
                "SELECT dolt_commit('-Am', 'shared');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "DELETE FROM shared;",
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
                    client: "writer",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
                    query: "INSERT INTO dolt_nonlocal_tables VALUES ('shared', 'main', '', 'immediate');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "writer",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
                    query: "INSERT INTO \"postgres/main\".public.shared VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "writer",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
                    query: "INSERT INTO \"postgres/main\".public.shared VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "writer",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_nonlocal_tables;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "writer",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM shared;",
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
            name: "nonlocal tables follow the search path, and a foreign key to a covered table found through it is refused",
            intent: "An unqualified name finds the rule of the first schema of the search path a rule matches it in, and a foreign key whose referenced name the search path resolves to a covered table is refused.",
            set_up_script: &[
                "CREATE TABLE x (pk int PRIMARY KEY);",
                "INSERT INTO x VALUES (1);",
                "CREATE TABLE other_name (pk int PRIMARY KEY);",
                "INSERT INTO other_name VALUES (7);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "DELETE FROM x;",
                "CREATE SCHEMA a;",
                "INSERT INTO dolt_nonlocal_tables VALUES ('public.x', 'main', '', 'immediate'), ('public.renamed_*', 'main', 'other_name', 'immediate');",
                "SET search_path = a, public;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM x;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM renamed_q;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a.child (pk int PRIMARY KEY REFERENCES x (pk));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "foreign keys referencing nonlocal table \"x\" are not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a.x (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a.child (pk int PRIMARY KEY REFERENCES x (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal tables in a schema the session's branch lacks have no name",
            intent: "A covered table of a schema the session's branch doesn't have isn't named in another schema.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE SCHEMA s2;",
                "CREATE TABLE s2.y (pk int PRIMARY KEY);",
                "INSERT INTO s2.y VALUES (1);",
                "SELECT dolt_commit('-Am', 'y in s2');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables VALUES ('y', 'main', '', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM s2.y;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"s2.y\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"y\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nonlocal rules match a quoted name with capitals as another name",
            intent: "Dolt lowercases a rule's pattern, which then matches names as Postgres resolves them, so a quoted name with capitals is a different table.",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables VALUES ('x', 'main', '', 'immediate');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"X\" (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x (pk int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot create table name x because it matches a name present in dolt_nonlocal_tables.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_revision_qualified_names_read_through_the_branchs_nonlocal_tables() {
    run_scripts(&[
        ScriptTest {
            name: "a name qualified by another branch reads through that branch's dolt_nonlocal_tables, as Dolt resolves it in the branch's database, and a revision read AS OF reads that commit's tables only",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk TEXT PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "SELECT dolt_commit('-Am', 'main');",
                "SELECT dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'rule');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO aliased_table VALUES ('second');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/other\".public.nonlocal_table ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", TEXT)],
                        rows: &[
                            &[T("amzmapqt")],
                            &[T("second")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/other\".public.nonlocal_table AS OF 'HEAD';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"public.nonlocal_table\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a name qualified by another branch reads through that branch's rules when the session's branch has rules of its own",
            set_up_script: &[
                "SELECT dolt_branch('other');",
                "CREATE TABLE aliased_table (pk TEXT PRIMARY KEY);",
                "INSERT INTO aliased_table VALUES ('amzmapqt');",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, ref_table, options) VALUES ('main_alias', 'other', 'nothing', 'immediate');",
                "SELECT dolt_commit('-Am', 'main');",
                "SELECT dolt_checkout('other');",
                "INSERT INTO dolt_nonlocal_tables (table_name, target_ref, ref_table, options) VALUES ('nonlocal_table', 'main', 'aliased_table', 'immediate');",
                "SELECT dolt_commit('-Am', 'rule');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/other\".public.nonlocal_table;",
                    expected: Expected::Rows { columns: &[Column("pk", TEXT)], rows: &[&[T("amzmapqt")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
