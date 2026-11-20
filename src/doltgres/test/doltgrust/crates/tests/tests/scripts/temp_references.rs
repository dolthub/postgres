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
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_temporary_references() {
    run_scripts(&[
        ScriptTest {
            name: "entries that reference temporary objects live and die with them",
            set_up_script: &[
                "CREATE TABLE ab1 (a INTEGER, b INTEGER, c INTEGER);",
                "CREATE TEMP TABLE tt (x INTEGER);",
                "CREATE TEMP SEQUENCE ts;",
                "CREATE TYPE pg_temp.tty AS (q INTEGER);",
                "CREATE FUNCTION pg_temp.tf(INTEGER) RETURNS INTEGER AS 'SELECT $1' LANGUAGE sql IMMUTABLE;",
                "CREATE STATISTICS pg_temp.stats_ext_temp ON a, b FROM ab1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT regexp_replace(pg_describe_object(tableoid, oid, 0), 'pg_temp_[0-9]*', 'pg_temp_REDACTED') AS descr, pg_statistics_obj_is_visible(oid) AS visible FROM pg_statistic_ext WHERE stxname = 'stats_ext_temp';",
                    expected: Expected::Rows {
                        columns: &[Column("descr", TEXT), Column("visible", BOOL)],
                        rows: &[
                            &[T("statistics object pg_temp_REDACTED.stats_ext_temp"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS stats_ext_temp;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"statistics object "stats_ext_temp" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS pg_temp.stats_ext_temp;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS pg_temp.s2 ON a, b FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE perm_def (a INTEGER DEFAULT nextval('ts'), b INTEGER);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CAST (INTEGER AS pg_temp.tty) WITH INOUT;",
                    expected: Expected::Tag("CREATE CAST"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE perm_check (a INTEGER CHECK (pg_temp.tf(a) > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO perm_def (b) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM perm_def ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxname FROM pg_statistic_ext ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME)],
                        rows: &[
                            &[T("s2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT castsource::regtype, casttarget::regtype::text LIKE 'pg_temp%' AS temp FROM pg_cast WHERE casttarget >= 16384;",
                    expected: Expected::Rows {
                        columns: &[Column("castsource", REGTYPE), Column("temp", BOOL)],
                        rows: &[
                            &[T("integer"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atthasdef FROM pg_attribute WHERE attrelid = 'perm_def'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atthasdef", BOOL)],
                        rows: &[
                            &[T("a"), T("t")],
                            &[T("b"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO perm_check VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "perm_check" violates check constraint "perm_check_a_check""#, detail: "Failing row contains (0).", schema: "public", table: "perm_check", constraint: "perm_check_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW tv AS SELECT x FROM tt;",
                    expected: Expected::Tag("CREATE VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "tv" will be a temporary view"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'tv';",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE perm_col (c pg_temp.tty);",
                    expected: Expected::Tag("CREATE TABLE"),
                    skip: Some("a permanent row can't depend on state that a crash deletes without a cascade, so a permanent relation can't depend on a temporary object (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW fv AS SELECT pg_temp.tf(1) AS one;",
                    expected: Expected::Tag("CREATE VIEW"),
                    skip: Some("a permanent row can't depend on state that a crash deletes without a cascade, so a permanent relation can't depend on a temporary object (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ab1_tf ON ab1 (pg_temp.tf(a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    skip: Some("a permanent row can't depend on state that a crash deletes without a cascade, so a permanent relation can't depend on a temporary object (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE perm_check;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CAST (INTEGER AS pg_temp.tty);",
                    expected: Expected::Tag("DROP CAST"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_cast WHERE casttarget >= 16384;",
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
            name: "another session's entries that reference a session's temporary objects live in that session's space",
            set_up_script: &[
                "CREATE TABLE ab1 (a INTEGER, b INTEGER);",
                "CREATE TEMP TABLE tt (x INTEGER);",
                "CREATE TEMP SEQUENCE ts;",
                "CREATE TYPE pg_temp.tty AS (q INTEGER);",
                "CREATE FUNCTION pg_temp.tf(INTEGER) RETURNS INTEGER AS 'SELECT $1' LANGUAGE sql;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ CREATE STATISTICS pg_temp_0.b_stats ON a, b FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE TABLE perm_b (a INT DEFAULT nextval('pg_temp_0.ts'));",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE CAST (BIGINT AS pg_temp_0.tty) WITH INOUT;",
                    expected: Expected::Tag("CREATE CAST"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE TABLE perm_d (a INT CHECK (pg_temp_0.tf(a) > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE VIEW pv AS SELECT * FROM pg_temp_0.tt;",
                    expected: Expected::Tag("CREATE VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "pv" will be a temporary view"#, ..N }],
                    client: "B",
                    skip: Some("a permanent row can't depend on state that a crash deletes without a cascade, so an object can't depend on the temporary objects of two sessions (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ CREATE TABLE perm_c (c pg_temp_0.tty);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "B",
                    skip: Some("a permanent row can't depend on state that a crash deletes without a cascade, so a permanent relation can't depend on a temporary object (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT classid::regclass AS catalog, count(*) FROM pg_depend WHERE refobjid IN (SELECT oid FROM pg_class WHERE relname = 'ts' UNION SELECT oid FROM pg_type WHERE typname = 'tty' UNION SELECT oid FROM pg_proc WHERE proname = 'tf') AND deptype = 'n' GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("catalog", REGCLASS), Column("count", INT8)],
                        rows: &[
                            &[T("pg_attrdef"), T("1")],
                            &[T("pg_cast"), T("1")],
                            &[T("pg_constraint"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT stxname FROM pg_statistic_ext ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME)],
                        rows: &[
                            &[T("b_stats")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_temporary_relations_of_another_session() {
    run_scripts(&[
        ScriptTest {
            name: "inheritance, partitions, and foreign keys refuse another session's temporary tables",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE other_t (x INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE other_p (x INT) PARTITION BY LIST (x);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE own_t (x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE own_p (x INT) PARTITION BY LIST (x);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('CREATE TEMP TABLE own_c () INHERITS (%I.other_t)', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'other_t')); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot inherit from temporary relation of another session", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('CREATE TEMP TABLE own_q PARTITION OF %I.other_p FOR VALUES IN (2)', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'other_t')); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot create as partition of temporary relation of another session", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('ALTER TABLE own_t INHERIT %I.other_t', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'other_t')); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot inherit from temporary relation of another session", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('ALTER TABLE own_p ATTACH PARTITION %I.other_t FOR VALUES IN (1)', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'other_t')); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot attach temporary relation of another session as partition", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN EXECUTE format('CREATE TEMP TABLE own_f (x INT REFERENCES %I.other_t)', (SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'other_t')); END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "constraints on temporary tables must involve temporary tables of this session", ..E }),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_oids_past_the_signed_range_and_temporary_objects() {
    run_scripts(&[
        ScriptTest {
            name: "OIDs past 2^31 compare, sort, key, and cast unsigned, and temporary objects resolve by name and OID",
            set_up_script: &[
                "CREATE TABLE oids (o OID PRIMARY KEY, n TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 3000000000::OID > 5::OID AS gt, 3000000000::OID::INT8 AS i8, 3000000000::OID::INT4 AS i4, (-1294967296)::INT4::OID AS back;",
                    expected: Expected::Rows {
                        columns: &[Column("gt", BOOL), Column("i8", INT8), Column("i4", INT4), Column("back", OID)],
                        rows: &[
                            &[T("t"), T("3000000000"), T("-1294967296"), T("3000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oids VALUES (3000000000, 'big'), (5, 'small'), (2147483648, 'edge'), (4294967295, 'max');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n FROM oids ORDER BY o;",
                    expected: Expected::Rows {
                        columns: &[Column("n", TEXT)],
                        rows: &[
                            &[T("small")],
                            &[T("edge")],
                            &[T("big")],
                            &[T("max")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n FROM oids WHERE o > 2147483647 ORDER BY o DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("n", TEXT)],
                        rows: &[
                            &[T("max")],
                            &[T("big")],
                            &[T("edge")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT min(o), max(o) FROM oids;",
                    expected: Expected::Rows {
                        columns: &[Column("min", OID), Column("max", OID)],
                        rows: &[
                            &[T("5"), T("4294967295")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 4294967295::OID::REGCLASS, 3000000000::REGTYPE, 3000000000::REGPROC;",
                    expected: Expected::Rows {
                        columns: &[Column("regclass", REGCLASS), Column("regtype", REGTYPE), Column("regproc", REGPROC)],
                        rows: &[
                            &[T("4294967295"), T("3000000000"), T("3000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tt (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'tt'::REGCLASS, to_regclass('tt') = 'tt'::REGCLASS AS same;",
                    expected: Expected::Rows {
                        columns: &[Column("regclass", REGCLASS), Column("same", BOOL)],
                        rows: &[
                            &[T("tt"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_class'::REGCLASS, 'tt'::REGCLASS, 0), pg_describe_object('pg_type'::REGCLASS, 'tt'::REGTYPE, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("table tt"), T("type tt")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname IN ('oids', 'tt') ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("oids")],
                            &[T("tt")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_table_privilege('tt'::REGCLASS, 'SELECT'), pg_relation_size('tt'::REGCLASS) >= 0 AS sized, pg_get_userbyid(relowner) IS NOT NULL AS owner FROM pg_class WHERE oid = 'tt'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("has_table_privilege", BOOL), Column("sized", BOOL), Column("owner", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type('tt'::REGTYPE, NULL), 'tt'::REGTYPE::OID = (SELECT reltype FROM pg_class WHERE oid = 'tt'::REGCLASS) AS rowtype;",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT), Column("rowtype", BOOL)],
                        rows: &[
                            &[T("tt"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_attribute WHERE attrelid = 'tt'::REGCLASS AND attnum > 0;",
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
                    query: "COMMENT ON TABLE tt IS 'temp';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('tt'::REGCLASS, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("temp")],
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

#[test]
fn test_temporary_tables_written_in_the_statement_that_makes_them() {
    run_scripts(&[
        ScriptTest {
            name: "a temporary table made in a statement is written, indexed, and copied in that statement",
            assertions: &[
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE TEMP TABLE made (id INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v TEXT); INSERT INTO made (v) VALUES ('a'), ('b'); CREATE INDEX made_v ON made (v); UPDATE made SET v = 'c' WHERE id = 2; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM made ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM made WHERE v = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE copied AS WITH w AS (SELECT id, v FROM made) SELECT id * 10 AS id, v FROM w;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM copied ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("10"), T("a")],
                            &[T("20"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO made (v) VALUES ('d') RETURNING id, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("d")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_temporary_schema_name() {
    run_scripts(&[
        ScriptTest {
            name: "a session's temporary schema is named for its backend's slot",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE slot_probe (i INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema();",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("pg_temp_2")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust numbers a session's temporary schema by its own slots from 0, where Postgres numbers it by the backend's proc number, 2 for this session (7329)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
