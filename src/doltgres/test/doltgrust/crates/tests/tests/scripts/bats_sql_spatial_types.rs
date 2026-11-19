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

//! Dolt's integration-tests/bats/sql-spatial-types.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_spatial_types() {
    run_scripts(&[
        ScriptTest {
            name: "sql-spatial-types: can make spatial types",
            intent: "A table can have a point column.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE point_tbl (p point);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: create point table and insert points",
            intent: "Points insert and read back.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE point_tbl (p point);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO point_tbl VALUES (point(1,2));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO point_tbl VALUES (point(3,4)), (point(5,6));",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO point_tbl VALUES (point(123.456, 0.789));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p::text FROM point_tbl ORDER BY p[0];",
                    expected: Expected::Rows {
                        columns: &[Column("p", TEXT)],
                        rows: &[
                            &[T("(1,2)")],
                            &[T("(3,4)")],
                            &[T("(5,6)")],
                            &[T("(123.456,0.789)")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's ST_ASWKT prints the well-known text, and a Postgres geometric value prints its own text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: create linestring table and insert linestrings",
            intent: "Open paths insert and read back.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE line_tbl (l path);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's LINESTRING is Postgres' open path")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO line_tbl VALUES ('[(1,2),(3,4)]');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO line_tbl VALUES ('[(1.2345,678.9),(111.222,333.444),(55.66,77.88)]'), ('[(1.1,2.2),(3.3,4)]');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l::text FROM line_tbl ORDER BY npoints(l), l::text;",
                    expected: Expected::Rows {
                        columns: &[Column("l", TEXT)],
                        rows: &[
                            &[T("[(1,2),(3,4)]")],
                            &[T("[(1.1,2.2),(3.3,4)]")],
                            &[T("[(1.2345,678.9),(111.222,333.444),(55.66,77.88)]")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's ST_ASWKT prints the well-known text, and a Postgres geometric value prints its own text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: create polygon table and insert polygon",
            intent: "Polygons insert and read back.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE poly_tbl (p polygon);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO poly_tbl VALUES (polygon(path '((1,2),(3,4),(5,6),(7,8))'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' polygon closes its own ring, so a ring whose last point differs from its first is a polygon too, where MySQL refuses it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO poly_tbl VALUES (polygon(path '((1,2),(3,4),(5,6),(1,2))'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO poly_tbl VALUES (polygon(path '((1,1),(2,2),(3,3),(1,1))')), (polygon(path '((0.123,0.456),(1.22,1.33),(1.11,0.99),(0.123,0.456))'));",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p::text FROM poly_tbl ORDER BY p::text;",
                    expected: Expected::Rows {
                        columns: &[Column("p", TEXT)],
                        rows: &[
                            &[T("((0.123,0.456),(1.22,1.33),(1.11,0.99),(0.123,0.456))")],
                            &[T("((1,1),(2,2),(3,3),(1,1))")],
                            &[T("((1,2),(3,4),(5,6),(1,2))")],
                            &[T("((1,2),(3,4),(5,6),(7,8))")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's ST_ASWKT prints the well-known text, and a Postgres geometric value prints its own text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: can create large geometry",
            intent: "A polygon of thousands of points, stored out of line, can be inserted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a polygon);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t SELECT ('(' || string_agg(format('(%s,%s)', i, i % 97), ',' ORDER BY i) || ')')::polygon FROM generate_series(1, 5000) i;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("Dolt's big_spatial.sql MULTIPOLYGON is MySQL's GEOMETRY, so the port builds a Postgres polygon of 5000 points, which is stored out of line as Dolt's is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), max(npoints(a)) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("max", INT4)],
                        rows: &[
                            &[T("1"), T("5000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: geometry survives dolt gc",
            intent: "A large polygon reads back the same after garbage collection.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE geom_tbl (g polygon);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY holding a point is a Postgres polygon here, large enough to be stored out of line, so dolt_gc must keep its chunks")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO geom_tbl SELECT ('(' || string_agg(format('(%s,%s)', i, i % 97), ',' ORDER BY i) || ')')::polygon FROM generate_series(1, 5000) i;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT npoints(g), md5(g::text) FROM geom_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("npoints", INT4), Column("md5", TEXT)],
                        rows: &[
                            &[T("5000"), T("6cf6f98d796deee8e7059cc4da4c0b07")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT npoints(g), md5(g::text) FROM geom_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("npoints", INT4), Column("md5", TEXT)],
                        rows: &[
                            &[T("5000"), T("6cf6f98d796deee8e7059cc4da4c0b07")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: geometry survives dolt push",
            intent: "A spatial value reads back the same after a push and clone.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE geom_tbl (g point);",
                    expected: Expected::Tag("?"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY holding a point is a Postgres point here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO geom_tbl VALUES (point(1,2));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'creating geometry table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g::text FROM geom_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT)],
                        rows: &[
                            &[T("(1,2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's ST_ASWKT prints the well-known text, and a Postgres geometric value prints its own text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'origin', 'file://rem1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_push('origin', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clone('file://rem1', 'repo2');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            skip: Some("the port never recorded this script's expectations (tag \"?\"); record them now that dolt_push exists"),
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: create geometry table and insert existing spatial types",
            intent: "A geometry column holds points, lines, and polygons.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE geom_tbl (g geometry);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY is PostGIS' geometry, and ST_ASWKT is ST_AsText")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO geom_tbl VALUES (ST_MakePoint(1,2));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO geom_tbl VALUES (ST_GeomFromText('LINESTRING(1 2,3 4)'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO geom_tbl VALUES (ST_GeomFromText('POLYGON((1 2,3 4,5 6,1 2))'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ST_AsText(g) FROM geom_tbl ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("st_astext", TEXT)],
                        rows: &[
                            &[T("LINESTRING(1 2,3 4)")],
                            &[T("POINT(1 2)")],
                            &[T("POLYGON((1 2,3 4,5 6,1 2))")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY is PostGIS' geometry, and ST_ASWKT is ST_AsText")),
                    ..A
                },
            ],
            skip: Some("PostGIS isn't ported (REBUILD_PLAN, pgvector and PostGIS); the expectations are written by hand in PostGIS form, to record on PG18 with PostGIS when lifted"),
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent point as primary key",
            intent: "A point column can't be a primary key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE point_tbl (p point PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type point has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent linestring as primary key",
            intent: "A path column can't be a primary key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE line_tbl (l path PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type path has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's LINESTRING is Postgres' open path")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent polygon as primary key",
            intent: "A polygon column can't be a primary key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE poly_tbl (p polygon PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type polygon has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent geometry as primary key",
            intent: "A geometry column can be a primary key, as PostGIS' btree class allows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE geom_tbl (g geometry PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("PostGIS' geometry has a btree operator class, so it can be a primary key, where MySQL refuses spatial types as keys")),
                    ..A
                },
            ],
            skip: Some("PostGIS isn't ported (REBUILD_PLAN, pgvector and PostGIS); the expectations are written by hand in PostGIS form, to record on PG18 with PostGIS when lifted"),
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent altering table to use point type as primary key",
            intent: "A primary key column can't be changed to a point.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE point_tbl (p int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE point_tbl ALTER COLUMN p TYPE point USING point(p, 0);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type point has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's MODIFY COLUMN ... PRIMARY KEY changes the key column's type, which Postgres converts with USING")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent altering table to use linestring type as primary key",
            intent: "A primary key column can't be changed to a path.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE line_tbl (l int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE line_tbl ALTER COLUMN l TYPE path USING path(polygon(box(point(l, 0), point(l, 1))));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type path has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's MODIFY COLUMN ... PRIMARY KEY changes the key column's type, which Postgres converts with USING, and MySQL's LINESTRING is Postgres' path")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent altering table to use polygon type as primary key",
            intent: "A primary key column can't be changed to a polygon.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE poly_tbl (p int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE poly_tbl ALTER COLUMN p TYPE polygon USING polygon(box(point(p, 0), point(p, 1)));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "data type polygon has no default operator class for access method \"btree\"", hint: "You must specify an operator class for the index or define a default operator class for the data type.", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's MODIFY COLUMN ... PRIMARY KEY changes the key column's type, which Postgres converts with USING")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: prevent altering table to use geometry type as primary key",
            intent: "A primary key column can be changed to geometry, as PostGIS' btree class allows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE geom_tbl (g int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE geom_tbl ALTER COLUMN g TYPE geometry USING ST_MakePoint(g, 0);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("PostGIS' geometry has a btree operator class, so it can be a primary key, where MySQL refuses spatial types as keys")),
                    ..A
                },
            ],
            skip: Some("PostGIS isn't ported (REBUILD_PLAN, pgvector and PostGIS); the expectations are written by hand in PostGIS form, to record on PG18 with PostGIS when lifted"),
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: allow index on non-spatial columns of spatial table",
            intent: "A table with a polygon column can index its other columns.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE poly_tbl (a int, p polygon);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON poly_tbl (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: SRID defined in column definition in CREATE TABLE",
            intent: "A column's SRID refuses a geometry of another SRID.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE pt (i int PRIMARY KEY, p geometry(Point, 4326) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's SRID column attribute is PostGIS' geometry(Point, srid) type, and the test's SRID 0 and its refusal of SRID 1 are MySQL's, so the port uses SRID 4326")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (1, ST_SetSRID(ST_MakePoint(5,6), 4326));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ST_AsText(p) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("st_astext", TEXT)],
                        rows: &[
                            &[T("POINT(5 6)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's GEOMETRY is PostGIS' geometry, and ST_ASWKT is ST_AsText")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (2, ST_SetSRID(ST_MakePoint(1,2), 3857));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Geometry SRID (3857) does not match column SRID (4326)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ST_AsText(p) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("st_astext", TEXT)],
                        rows: &[
                            &[T("POINT(5 6)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_table_name, f_geometry_column, srid, type FROM geometry_columns WHERE f_table_name = 'pt';",
                    expected: Expected::Rows {
                        columns: &[Column("f_table_name", NAME), Column("f_geometry_column", NAME), Column("srid", INT4), Column("type", VARCHAR)],
                        rows: &[
                            &[T("pt"), T("p"), T("4326"), T("POINT")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("information_schema.ST_GEOMETRY_COLUMNS is MySQL's, so PostGIS' geometry_columns lists the column")),
                    ..A
                },
            ],
            skip: Some("PostGIS isn't ported (REBUILD_PLAN, pgvector and PostGIS); the expectations are written by hand in PostGIS form, to record on PG18 with PostGIS when lifted"),
            ..S
        },
        ScriptTest {
            name: "sql-spatial-types: SRID defined in column definition in ALTER TABLE",
            intent: "Changing a geometry column's type or SRID refuses rows that don't fit it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE table1 (i int PRIMARY KEY, p geometry(LineString, 4326) NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's SRID column attribute is PostGIS' geometry(type, srid) type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table1 VALUES (1, ST_SetSRID(ST_GeomFromText('LINESTRING(0 0,1 2)'), 4326));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE table1 ALTER COLUMN p TYPE geometry(Geometry, 4326);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's MODIFY COLUMN is ALTER COLUMN ... TYPE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table1 VALUES (2, ST_SetSRID(ST_MakePoint(2,1), 4326));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ST_AsText(p) FROM table1 ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("st_astext", TEXT)],
                        rows: &[
                            &[T("LINESTRING(0 0,1 2)")],
                            &[T("POINT(2 1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE table1 ALTER COLUMN p TYPE geometry(LineString, 4326);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Geometry type (Point) does not match column type (LineString)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM table1 WHERE i = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE table1 ALTER COLUMN p TYPE geometry(Point, 4326);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE table1 ALTER COLUMN p TYPE geometry(Point, 3857);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Geometry SRID (4326) does not match column SRID (3857)", ..E }),
                    ..A
                },
            ],
            skip: Some("PostGIS isn't ported (REBUILD_PLAN, pgvector and PostGIS); the expectations are written by hand in PostGIS form, to record on PG18 with PostGIS when lifted"),
            ..S
        },
    ]);
}
