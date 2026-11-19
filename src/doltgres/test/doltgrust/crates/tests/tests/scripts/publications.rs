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

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, Flow, S, ScriptTest, ScriptTestAssertion, run_scripts};


#[test]
fn test_publication_ddl() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE, ALTER, and DROP PUBLICATION",
            set_up_script: &[
                "SET client_min_messages = 'ERROR';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_default;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_insert WITH (publish = 'insert', publish_via_partition_root = true, publish_generated_columns = stored);",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pubname, puballtables, pubinsert, pubupdate, pubdelete, pubtruncate, pubviaroot, pubgencols FROM pg_publication ORDER BY pubname;",
                    expected: Expected::Rows {
                        columns: &[Column("pubname", NAME), Column("puballtables", BOOL), Column("pubinsert", BOOL), Column("pubupdate", BOOL), Column("pubdelete", BOOL), Column("pubtruncate", BOOL), Column("pubviaroot", BOOL), Column("pubgencols", CHAR)],
                        rows: &[
                            &[T("pub_default"), T("f"), T("t"), T("t"), T("t"), T("t"), T("f"), T("n")],
                            &[T("pub_insert"), T("f"), T("t"), T("f"), T("f"), T("f"), T("t"), T("s")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_default;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"publication "pub_default" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_bad WITH (foo);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized publication parameter: "foo""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_bad WITH (publish = 'cluster, vacuum');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized value for publication option "publish": "cluster""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_bad WITH (publish_via_partition_root = 'true', publish_via_partition_root = '0');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 71, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_bad WITH (publish_generated_columns = foo);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"invalid value for publication parameter "publish_generated_columns": "foo""#, detail: r#"Valid values are "none" and "stored"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_bad WITH (publish_generated_columns);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"invalid value for publication parameter "publish_generated_columns": """#, detail: r#"Valid values are "none" and "stored"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_default SET (publish = 'insert, update, delete');",
                    expected: Expected::Tag("ALTER PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pubinsert, pubupdate, pubdelete, pubtruncate FROM pg_publication WHERE pubname = 'pub_default';",
                    expected: Expected::Rows {
                        columns: &[Column("pubinsert", BOOL), Column("pubupdate", BOOL), Column("pubdelete", BOOL), Column("pubtruncate", BOOL)],
                        rows: &[&[T("t"), T("t"), T("t"), T("f")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_default RENAME TO pub_insert;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"publication "pub_insert" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_default RENAME TO pub_renamed;",
                    expected: Expected::Tag("ALTER PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_default SET (publish = 'insert');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"publication "pub_default" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_publication'::regclass, oid, 0), (pg_identify_object('pg_publication'::regclass, oid, 0)).identity FROM pg_publication ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("identity", TEXT)],
                        rows: &[&[T("publication pub_insert"), T("pub_insert")], &[T("publication pub_renamed"), T("pub_renamed")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PUBLICATION pub_renamed, pub_insert;",
                    expected: Expected::Tag("DROP PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PUBLICATION pub_renamed;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"publication "pub_renamed" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP PUBLICATION IF EXISTS pub_renamed;",
                    expected: Expected::Tag("DROP PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_publication;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE PUBLICATION warns while wal_level is not logical",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_warned;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    notices: &[Diagnostic {
                        severity: "WARNING",
                        code: "55000",
                        message: r#""wal_level" is insufficient to publish logical changes"#,
                        hint: r#"Set "wal_level" to "logical" before creating subscriptions."#,
                        ..E
                    }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "publication tables and schemas",
            set_up_script: &[
                "SET client_min_messages = 'ERROR';",
                "CREATE SCHEMA pub_s;",
                "CREATE TABLE pub_s.t1 (a int PRIMARY KEY, b text);",
                "CREATE TABLE pub_t2 (a int PRIMARY KEY);",
                "CREATE VIEW pub_v AS SELECT 1 AS x;",
                "CREATE TEMP TABLE pub_temp (a int);",
                "CREATE UNLOGGED TABLE pub_unlogged (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_all FOR ALL TABLES;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_all ADD TABLE pub_t2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"publication "pub_all" is defined as FOR ALL TABLES"#, detail: "Tables cannot be added to or dropped from FOR ALL TABLES publications.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_all ADD TABLES IN SCHEMA pub_s;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"publication "pub_all" is defined as FOR ALL TABLES"#, detail: "Schemas cannot be added to or dropped from FOR ALL TABLES publications.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_tables FOR TABLE pub_t2, TABLES IN SCHEMA pub_s;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pubname, schemaname, tablename, attnames, rowfilter FROM pg_publication_tables ORDER BY pubname, tablename;",
                    expected: Expected::Rows {
                        columns: &[Column("pubname", NAME), Column("schemaname", NAME), Column("tablename", NAME), Column("attnames", NAME_ARRAY), Column("rowfilter", TEXT)],
                        rows: &[
                            &[T("pub_all"), T("public"), T("pub_t2"), T("{a}"), Null],
                            &[T("pub_all"), T("pub_s"), T("t1"), T("{a,b}"), Null],
                            &[T("pub_tables"), T("public"), T("pub_t2"), T("{a}"), Null],
                            &[T("pub_tables"), T("pub_s"), T("t1"), T("{a,b}"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, 0), (pg_identify_object(classid, objid, 0)).identity FROM pg_depend WHERE classid IN ('pg_publication_rel'::regclass, 'pg_publication_namespace'::regclass) AND deptype = 'a' AND refclassid <> 'pg_publication'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("publication of schema pub_s in publication pub_tables"), T("pub_s in publication pub_tables")],
                            &[T("publication of table pub_t2 in publication pub_tables"), T("public.pub_t2 in publication pub_tables")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLE pub_t2;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"relation "pub_t2" is already member of publication "pub_tables""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLES IN SCHEMA pub_s;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"schema "pub_s" is already member of publication "pub_tables""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLE pub_v;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot add relation "pub_v" to publication"#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLE pub_temp;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot add relation "pub_temp" to publication"#, detail: "This operation is not supported for temporary tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLE pub_unlogged;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot add relation "pub_unlogged" to publication"#, detail: "This operation is not supported for unlogged tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLE pg_class;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot add relation "pg_class" to publication"#, detail: "This operation is not supported for system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables ADD TABLES IN SCHEMA pg_catalog;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot add schema "pg_catalog" to publication"#, detail: "This operation is not supported for system schemas.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pub_t2 SET UNLOGGED;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot change table "pub_t2" to unlogged because it is part of a publication"#, detail: "Unlogged relations cannot be replicated.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables DROP TABLE pub_s.t1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"relation "t1" is not part of the publication"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables DROP TABLES IN SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"tables from schema "public" are not part of the publication"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_tables SET TABLE pub_s.t1;",
                    expected: Expected::Tag("ALTER PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM pg_publication_rel), (SELECT count(*) FROM pg_publication_namespace);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[&[T("1"), T("0")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE pub_s.t1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_publication_rel;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_publication_tables('pub_tables', 'no_such_pub');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"publication "no_such_pub" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "publication row filters and column lists",
            set_up_script: &[
                "SET client_min_messages = 'ERROR';",
                "CREATE TABLE rf (a int PRIMARY KEY, b int, c text);",
                "CREATE TABLE rf_nopk (a int, b int);",
                "INSERT INTO rf VALUES (1, 1, 'x');",
                "INSERT INTO rf_nopk VALUES (1, 1);",
                "CREATE FUNCTION rf_func(int) RETURNS bool LANGUAGE sql IMMUTABLE AS 'SELECT $1 > 0';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf WHERE (a);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of PUBLICATION WHERE must be type boolean, not type integer", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf WHERE (rf_func(a));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid publication WHERE expression", detail: "User-defined or built-in mutable functions are not allowed.", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf WHERE (a < random());",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid publication WHERE expression", detail: "User-defined or built-in mutable functions are not allowed.", position: 51, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf WHERE (ctid = '(0,1)');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid publication WHERE expression", detail: "System columns are not allowed.", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf WHERE (a IN (SELECT 1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid publication WHERE expression", detail: "Only columns, constants, built-in operators, built-in data types, built-in collations, and immutable built-in functions are allowed.", position: 47, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf (a, a);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"duplicate column "a" in publication column list"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf (a, ctid);",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"cannot use system column "ctid" in publication column list"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf (a, x);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "x" of relation "rf" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_bad FOR TABLE rf (a), TABLES IN SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot use column list for relation "public.rf" in publication "rf_bad""#, detail: "Column lists cannot be specified in publications containing FOR TABLES IN SCHEMA elements.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION rf_pub FOR TABLE rf (a, c) WHERE (b > 0);",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT prattrs, pg_get_expr(prqual, prrelid) FROM pg_publication_rel;",
                    expected: Expected::Rows {
                        columns: &[Column("prattrs", INT2VECTOR), Column("pg_get_expr", TEXT)],
                        rows: &[&[T("1 3"), T("(b > 0)")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rf SET c = 'y';",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"cannot update table "rf""#, detail: "Column used in the publication WHERE expression is not part of the replica identity.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rf;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"cannot delete from table "rf""#, detail: "Column used in the publication WHERE expression is not part of the replica identity.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rf DROP COLUMN c;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop column c of table rf because other objects depend on it", detail: "publication of table rf in publication rf_pub depends on column c of table rf", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rf ALTER COLUMN b TYPE bigint;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a publication WHERE clause", detail: r#"publication of table rf in publication rf_pub depends on column "b""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION rf_pub SET TABLE rf (b, c);",
                    expected: Expected::Tag("ALTER PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rf SET c = 'y';",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"cannot update table "rf""#, detail: "Column list used by the publication does not cover the replica identity.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION rf_pub SET TABLE rf_nopk;",
                    expected: Expected::Tag("ALTER PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rf SET c = 'y';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rf_nopk SET b = 2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot update table "rf_nopk" because it does not have a replica identity and publishes updates"#, hint: "To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rf_nopk;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot delete from table "rf_nopk" because it does not have a replica identity and publishes deletes"#, hint: "To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rf_nopk VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rf_nopk REPLICA IDENTITY FULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rf_nopk WHERE a = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION rf_pub DROP TABLE rf_nopk WHERE (a > 0);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot use a WHERE clause when removing a table from a publication", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "publication ownership",
            set_up_script: &[
                "SET client_min_messages = 'ERROR';",
                "CREATE ROLE pub_owner LOGIN PASSWORD 'password';",
                "CREATE ROLE pub_other;",
                "GRANT CREATE ON DATABASE postgres TO pub_owner;",
                "CREATE TABLE pub_owned (a int PRIMARY KEY);",
                "ALTER TABLE pub_owned OWNER TO pub_owner;",
                "CREATE SCHEMA pub_owned_s;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_by_owner FOR TABLE pub_owned;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    notices: &[Diagnostic {
                        severity: "WARNING",
                        code: "55000",
                        message: r#""wal_level" is insufficient to publish logical changes"#,
                        hint: r#"Set "wal_level" to "logical" before creating subscriptions."#,
                        ..E
                    }],
                    username: "pub_owner",
                    password: "password",
                    client: "pub_owner",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_by_owner_all FOR ALL TABLES;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create FOR ALL TABLES publication", ..E }),
                    username: "pub_owner",
                    password: "password",
                    client: "pub_owner",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_by_owner_schema FOR TABLES IN SCHEMA pub_owned_s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create FOR TABLES IN SCHEMA publication", ..E }),
                    username: "pub_owner",
                    password: "password",
                    client: "pub_owner",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_by_owner OWNER TO pub_other;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be able to SET ROLE "pub_other""#, ..E }),
                    username: "pub_owner",
                    password: "password",
                    client: "pub_owner",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PUBLICATION pub_super FOR ALL TABLES;",
                    expected: Expected::Tag("CREATE PUBLICATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER PUBLICATION pub_super RENAME TO pub_super2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of publication pub_super", ..E }),
                    username: "pub_owner",
                    password: "password",
                    client: "pub_owner",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE pub_owner;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "pub_owner" cannot be dropped because some objects depend on it"#, detail: "privileges for database postgres\nowner of table pub_owned\nowner of publication pub_by_owner", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY pub_owner TO pub_other;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pubname, pg_get_userbyid(pubowner) FROM pg_publication ORDER BY pubname;",
                    expected: Expected::Rows {
                        columns: &[Column("pubname", NAME), Column("pg_get_userbyid", NAME)],
                        rows: &[&[T("pub_by_owner"), T("pub_other")], &[T("pub_super"), T("postgres")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY pub_other;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pubname FROM pg_publication ORDER BY pubname;",
                    expected: Expected::Rows { columns: &[Column("pubname", NAME)], rows: &[&[T("pub_super")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_publication_tables_after_waiting_for_a_concurrent_drop() {
    run_scripts(&[
        ScriptTest {
            name: "listing a publication's tables waits for a table's lock and leaves the table out once it is dropped",
            set_up_script: &[
                "CREATE SCHEMA pubdrop;",
                "CREATE PUBLICATION pub_schema FOR TABLES IN SCHEMA pubdrop;",
                "CREATE TABLE pubdrop.dropme (id INT);",
                "CREATE TABLE pubdrop.keepme (id INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK pubdrop.dropme IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid::regclass AS tablename FROM pg_get_publication_tables('pub_schema') ORDER BY tablename;",
                    expected: Expected::Rows {
                        columns: &[Column("tablename", REGCLASS)],
                        rows: &[&[T("pubdrop.keepme")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE pubdrop.dropme;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "listing a publication's tables releases the lock it took on a table dropped while it waited",
            set_up_script: &[
                "CREATE SCHEMA pubdrop;",
                "CREATE PUBLICATION pub_schema FOR TABLES IN SCHEMA pubdrop;",
                "CREATE TABLE pubdrop.dropme (id INT);",
                "CREATE TABLE pubdrop.keepme (id INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK pubdrop.dropme IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid::regclass AS tablename FROM pg_get_publication_tables('pub_schema') ORDER BY tablename;",
                    expected: Expected::Rows {
                        columns: &[Column("tablename", REGCLASS)],
                        rows: &[&[T("pubdrop.keepme")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE pubdrop.dropme;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) AS dropped FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND NOT EXISTS (SELECT 1 FROM pg_class WHERE oid = relation);",
                    expected: Expected::Rows {
                        columns: &[Column("dropped", INT8)],
                        rows: &[&[T("0")]],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
