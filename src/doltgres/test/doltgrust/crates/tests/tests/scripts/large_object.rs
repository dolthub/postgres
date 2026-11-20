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

//! Large objects: their pages in `pg_largeobject`, their owners and privileges in `pg_largeobject_metadata` and
//! `pg_shdepend`, the functions that read and write them through descriptors, and the commands on them.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_large_objects() {
    run_scripts(&[
        ScriptTest {
            name: "large objects through their functions, descriptors, privileges, and shared dependencies",
            set_up_script: &[
                "CREATE ROLE lo_owner;",
                "CREATE ROLE lo_reader;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT lo_create(5001);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_create", OID)],
                        rows: &[
                            &[T("5001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_put(5001, 0, 'hello world');",
                    expected: Expected::Rows {
                        columns: &[Column("lo_put", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loid, pageno, data FROM pg_largeobject WHERE loid = 5001;",
                    expected: Expected::Rows {
                        columns: &[Column("loid", OID), Column("pageno", INT4), Column("data", BYTEA)],
                        rows: &[
                            &[T("5001"), T("0"), T("\\x68656c6c6f20776f726c64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid, lomowner::regrole, lomacl FROM pg_largeobject_metadata WHERE oid = 5001;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("lomowner", REGROLE), Column("lomacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("5001"), T("postgres"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_create(5001);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"pg_largeobject_metadata_oid_index\"", detail: "Key (oid)=(5001) already exists.", schema: "pg_catalog", table: "pg_largeobject_metadata", constraint: "pg_largeobject_metadata_oid_index", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_get(5002);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "large object 5002 does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_get(5001, 6, -1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "requested length cannot be negative", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_get(5001, 6, 100), lo_get(5001, 100, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_get", BYTEA), Column("lo_get", BYTEA)],
                        rows: &[
                            &[T("\\x776f726c64"), T("\\x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid flags for opening a large object: 0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_close(99), loread(99, 1);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "invalid large-object descriptor: 99", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 262144);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_open", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lowrite(0, 'x');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "large object descriptor 0 was not opened for writing", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_truncate(0, 1);",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_lseek(0, 0, 5);",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 262144);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_open", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_lseek64(0, -1, 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid large object seek target: -1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_lseek64(0, 3, 0), lo_tell64(0), loread(0, 4), lo_tell(0);",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_lseek(0, -2, 1), loread(0, 100);",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loread(0, 1);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "invalid large-object descriptor: 0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 262144);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_open", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 393216);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_open", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loread(1, 1);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "invalid large-object descriptor: 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loread(0, 5);",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_open(5001, 262144);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_open", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_put(5001, 0, 'HELLO');",
                    expected: Expected::Rows {
                        columns: &[Column("lo_put", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT loread(0, 5), lo_get(5001, 0, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("loread", BYTEA), Column("lo_get", BYTEA)],
                        rows: &[
                            &[T("\\x68656c6c6f"), T("\\x48454c4c4f")],
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
                    query: "SELECT lo_from_bytea(5003, 'data');",
                    expected: Expected::Rows {
                        columns: &[Column("lo_from_bytea", OID)],
                        rows: &[
                            &[T("5003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_import('{TESTDATA}/large-object.txt', 5004);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_import", OID)],
                        rows: &[
                            &[T("5004")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_get(5004);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_get", BYTEA)],
                        rows: &[
                            &[T("\\x6162636465666768696a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_export(5004, '{TEMPDIR}/large-object-out.txt');",
                    expected: Expected::Rows {
                        columns: &[Column("lo_export", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_import('{TEMPDIR}/missing.txt');",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not open server file \"{TEMPDIR}/missing.txt\": No such file or directory", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LARGE OBJECT 5001 OWNER TO lo_owner;",
                    expected: Expected::Tag("ALTER LARGE OBJECT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON LARGE OBJECT 5001 TO lo_reader;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE ON LARGE OBJECT 5002 TO lo_reader;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "large object 5002 does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid, lomowner::regrole, lomacl FROM pg_largeobject_metadata ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("lomowner", REGROLE), Column("lomacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("5001"), T("lo_owner"), T("{lo_owner=rw/lo_owner,lo_reader=r/lo_owner}")],
                            &[T("5003"), T("postgres"), Null],
                            &[T("5004"), T("postgres"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid, objsubid, refobjid::regrole, deptype FROM pg_shdepend WHERE classid = 'pg_largeobject'::regclass ORDER BY objid, deptype, refobjid::regrole::text;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("objid", OID), Column("objsubid", INT4), Column("refobjid", REGROLE), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_largeobject"), T("5001"), T("0"), T("lo_reader"), T("a")],
                            &[T("pg_largeobject"), T("5001"), T("0"), T("lo_owner"), T("o")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_largeobject_privilege('lo_reader', 5001, 'SELECT'), has_largeobject_privilege('lo_reader', 5001, 'UPDATE'), has_largeobject_privilege('lo_reader', 5009, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_largeobject_privilege", BOOL), Column("has_largeobject_privilege", BOOL), Column("has_largeobject_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_largeobject'::regclass, 5001, 0), pg_identify_object('pg_largeobject'::regclass, 5001, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_identify_object", RECORD)],
                        rows: &[
                            &[T("large object 5001"), T("(\"large object\",,,5001)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_object_address('large object', '{5001}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("classid", OID), Column("objid", OID), Column("objsubid", INT4)],
                        rows: &[
                            &[T("2613"), T("5001"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_object_address('large object', '{5009}', '{}');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "large object 5009 does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON LARGE OBJECT 5001 IS 'a comment';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(5001, 'pg_largeobject');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a comment")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE lo_reader;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"lo_reader\" cannot be dropped because some objects depend on it", detail: "privileges for large object 5001", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE lo_owner;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"lo_owner\" cannot be dropped because some objects depend on it", detail: "owner of large object 5001", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE lo_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_get(5001);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_get", BYTEA)],
                        rows: &[
                            &[T("\\x48454c4c4f20776f726c64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_put(5001, 0, 'x');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for large object 5001", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_unlink(5001);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of large object 5001", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lo_compat_privileges = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE lo_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_put(5003, 0, 'D');",
                    expected: Expected::Rows {
                        columns: &[Column("lo_put", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET lo_compat_privileges;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY lo_owner TO lo_reader;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid, lomowner::regrole FROM pg_largeobject_metadata WHERE oid = 5001;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("lomowner", REGROLE)],
                        rows: &[
                            &[T("5001"), T("lo_reader")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY lo_reader;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid FROM pg_largeobject_metadata ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID)],
                        rows: &[
                            &[T("5003")],
                            &[T("5004")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(5001, 'pg_largeobject');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_unlink(5003), lo_unlink(5004);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_unlink", INT4), Column("lo_unlink", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_unlink(5003);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "large object 5003 does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_largeobject;",
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
                    query: "DROP ROLE lo_reader;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE lo_owner;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a large object that shares its OID with a table is told apart from it by its catalog everywhere objects are named",
            set_up_script: &[
                "CREATE ROLE lo_c_a;",
                "CREATE ROLE lo_c_b;",
                "CREATE TABLE collide (a INT);",
                "ALTER TABLE collide OWNER TO lo_c_a;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT lo_create('collide'::regclass::oid) = 'collide'::regclass::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('ALTER LARGE OBJECT %s OWNER TO lo_c_b', 'collide'::regclass::oid); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('GRANT SELECT ON LARGE OBJECT %s TO lo_c_a', 'collide'::regclass::oid); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON collide TO lo_c_b;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('COMMENT ON LARGE OBJECT %s IS %L', 'collide'::regclass::oid, 'the large object'); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE collide IS 'the table';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('collide'::regclass, 'pg_class'), obj_description('collide'::regclass::oid, 'pg_largeobject');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("the table"), T("the large object")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_class'::regclass, 'collide'::regclass, 0), pg_describe_object('pg_largeobject'::regclass, 'collide'::regclass, 0) = 'large object ' || 'collide'::regclass::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("table collide"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_largeobject'::regclass, 'collide'::regclass, 0)).identity = 'collide'::regclass::oid::text AS lo, (pg_identify_object('pg_class'::regclass, 'collide'::regclass, 0)).identity, (pg_identify_object('pg_largeobject'::regclass, 'collide'::regclass, 0)).type;",
                    expected: Expected::Rows {
                        columns: &[Column("lo", BOOL), Column("identity", TEXT), Column("type", TEXT)],
                        rows: &[
                            &[T("t"), T("public.collide"), T("large object")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid = 'collide'::regclass::oid AS same, deptype, refobjid::regrole FROM pg_shdepend WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) ORDER BY classid::regclass::text, deptype;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("same", BOOL), Column("deptype", CHAR), Column("refobjid", REGROLE)],
                        rows: &[
                            &[T("pg_class"), T("t"), T("a"), T("lo_c_b")],
                            &[T("pg_class"), T("t"), T("o"), T("lo_c_a")],
                            &[T("pg_largeobject"), T("t"), T("a"), T("lo_c_a")],
                            &[T("pg_largeobject"), T("t"), T("o"), T("lo_c_b")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('REVOKE SELECT ON LARGE OBJECT %s FROM lo_c_a', 'collide'::regclass::oid); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('lo_c_b', 'collide', 'SELECT'), has_largeobject_privilege('lo_c_a', 'collide'::regclass::oid, 'SELECT');",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("has_largeobject_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid = 'collide'::regclass::oid AS same, deptype, refobjid::regrole FROM pg_shdepend WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) ORDER BY classid::regclass::text, deptype;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("same", BOOL), Column("deptype", CHAR), Column("refobjid", REGROLE)],
                        rows: &[
                            &[T("pg_class"), T("t"), T("a"), T("lo_c_b")],
                            &[T("pg_class"), T("t"), T("o"), T("lo_c_a")],
                            &[T("pg_largeobject"), T("t"), T("o"), T("lo_c_b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY lo_c_b;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_largeobject_metadata;",
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
                    query: "SELECT relname, relacl, obj_description(oid, 'pg_class'), obj_description(oid, 'pg_largeobject') FROM pg_class WHERE oid = 'collide'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relacl", ACLITEM_ARRAY), Column("obj_description", TEXT), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("collide"), T("{lo_c_a=arwdDxtm/lo_c_a}"), T("the table"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid = 'collide'::regclass::oid AS same, deptype, refobjid::regrole FROM pg_shdepend WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) ORDER BY classid::regclass::text, deptype;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("same", BOOL), Column("deptype", CHAR), Column("refobjid", REGROLE)],
                        rows: &[
                            &[T("pg_class"), T("t"), T("o"), T("lo_c_a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_create('collide'::regclass::oid) = 'collide'::regclass::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY lo_c_a TO lo_c_b;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relowner::regrole, (SELECT lomowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) FROM pg_largeobject_metadata WHERE oid = 'collide'::regclass::oid) AS kept FROM pg_class WHERE oid = 'collide'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relowner", REGROLE), Column("kept", BOOL)],
                        rows: &[
                            &[T("lo_c_b"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_unlink('collide'::regclass::oid);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_unlink", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, obj_description(oid, 'pg_class') FROM pg_class WHERE oid = 'collide'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("collide"), T("the table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_create('collide'::regclass::oid) = 'collide'::regclass::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('COMMENT ON LARGE OBJECT %s IS %L', 'collide'::regclass::oid, 'the large object'); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE collide;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), min(obj_description(oid, 'pg_largeobject')) FROM pg_largeobject_metadata;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("min", TEXT)],
                        rows: &[
                            &[T("1"), T("the large object")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_description WHERE objsubid = 0 AND description = 'the table';",
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
                    query: "SELECT lo_unlink(oid) FROM pg_largeobject_metadata;",
                    expected: Expected::Rows {
                        columns: &[Column("lo_unlink", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE lo_c_a;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE lo_c_b;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_identify_a_missing_large_object() {
    run_scripts(&[
        ScriptTest {
            name: "the object functions give NULL names for a large object that does not exist",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_largeobject'::regclass, 0, 0), pg_identify_object('pg_largeobject'::regclass, 0, 0)::text, pg_identify_object_as_address('pg_largeobject'::regclass, 0, 0)::text;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_identify_object", TEXT), Column("pg_identify_object_as_address", TEXT)],
                        rows: &[
                            &[Null, T(r#"("large object",,,)"#), T(r#"("large object",,)"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lo_create(4242);",
                    expected: Expected::Rows {
                        columns: &[Column("lo_create", OID)],
                        rows: &[
                            &[T("4242")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_largeobject'::regclass, 4242, 0), pg_identify_object('pg_largeobject'::regclass, 4242, 0)::text, pg_identify_object_as_address('pg_largeobject'::regclass, 4242, 0)::text;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_identify_object", TEXT), Column("pg_identify_object_as_address", TEXT)],
                        rows: &[
                            &[T("large object 4242"), T(r#"("large object",,,4242)"#), T(r#"("large object",{4242},{})"#)],
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
