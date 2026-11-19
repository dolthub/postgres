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

//! Tests of SELECT statements over the simple query protocol.

use harness::conn::connect;
use harness::pgx::Conn;
use harness::server::{Server, Target};

#[test]
fn test_select_without_a_from_clause_returns_its_row() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let result = conn.simple_query_rows("SELECT 1 + 2 AS a, 'x' || 'y', NULL::int").unwrap();
    let fields: Vec<(&str, u32, i16, i32)> = result
        .fields
        .iter()
        .map(|f| (f.name.as_str(), f.data_type_oid, f.data_type_size, f.type_modifier))
        .collect();
    assert_eq!(fields, [("a", 23, 4, -1), ("?column?", 25, -1, -1), ("int4", 23, 4, -1)]);
    assert_eq!(result.rows, [vec![Some(b"3".to_vec()), Some(b"xy".to_vec()), None]]);
    assert_eq!(result.command_tag, "SELECT 1");
    let result = conn.simple_query_rows("SELECT 1 WHERE 1 = 2").unwrap();
    assert_eq!(result.fields.len(), 1);
    assert!(result.rows.is_empty());
    assert_eq!(result.command_tag, "SELECT 0");
    let err = conn.simple_query_rows("SELECT 1 / 0").unwrap_err();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("22012", "division by zero"));
}

#[test]
fn test_select_calls_the_sequence_functions() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE SEQUENCE s", &[]).unwrap();
    let sql = "SELECT nextval('s'), currval('s'), lastval(), setval('s', 10), nextval('s'), setval('s', 5, false), nextval('s')";
    let result = conn.simple_query_rows(sql).unwrap();
    let row: Vec<String> = result.rows[0].iter().map(|v| String::from_utf8(v.clone().unwrap()).unwrap()).collect();
    assert_eq!(row.join("|"), "1|1|1|10|11|5|5");
    let err = conn.simple_query_rows("SELECT nextval('pg_class')").unwrap().error.unwrap();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("42809", "cannot open relation \"pg_class\""));
    assert_eq!(fields.detail, "This operation is not supported for tables.");
    let mut other = connect(&server, "postgres", "postgres");
    let err = other.simple_query_rows("SELECT lastval()").unwrap().error.unwrap();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("55000", "lastval is not yet defined in this session"));
    let result = other.simple_query_rows("SELECT nextval('s')").unwrap();
    assert_eq!(result.rows, [vec![Some(b"6".to_vec())]]);
}

#[test]
fn test_select_reads_the_sql_value_functions() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let sql = "SELECT current_user, session_user, current_catalog, current_schema, current_date = now()::date, \
               current_timestamp = now(), localtimestamp = now()::timestamp, CURRENT_TIME(0) IS NOT NULL, \
               LOCALTIME IS NOT NULL, current_timestamp > '2026-01-01'";
    let result = conn.simple_query_rows(sql).unwrap();
    let row: Vec<String> = result.rows[0].iter().map(|v| String::from_utf8(v.clone().unwrap()).unwrap()).collect();
    assert_eq!(row.join("|"), "postgres|postgres|postgres|public|t|t|t|t|t|t");
}

#[test]
fn test_select_reads_a_sequence_last_value() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE SEQUENCE lv", &[]).unwrap();
    conn.exec("CREATE SEQUENCE lvc CACHE 10", &[]).unwrap();
    let result = conn.simple_query_rows("SELECT pg_sequence_last_value('lv')").unwrap();
    assert_eq!(result.rows, [vec![None]]);
    conn.simple_query_rows("SELECT nextval('lv'), nextval('lvc')").unwrap();
    let result = conn.simple_query_rows("SELECT pg_sequence_last_value('lv'), pg_sequence_last_value('lvc')").unwrap();
    assert_eq!(result.rows, [vec![Some(b"1".to_vec()), Some(b"10".to_vec())]]);
}

#[test]
fn test_select_reads_a_table() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE st (id int PRIMARY KEY, name text, n int NOT NULL)", &[]).unwrap();
    let result = conn.simple_query_rows("SELECT name, n + 1 AS m, * FROM st WHERE n > 5 OR name IS NULL").unwrap();
    let fields: Vec<(&str, u32, u16)> =
        result.fields.iter().map(|f| (f.name.as_str(), f.data_type_oid, f.table_attribute_number)).collect();
    assert_eq!(fields, [("name", 25, 2), ("m", 23, 0), ("id", 23, 1), ("name", 25, 2), ("n", 23, 3)]);
    assert!(result.rows.is_empty());
    assert_eq!(result.command_tag, "SELECT 0");
    let result = conn.simple_query_rows("SELECT 1 FROM st WHERE false").unwrap();
    assert_eq!(result.command_tag, "SELECT 0");
    let err = conn.simple_query_rows("SELECT missing FROM st").unwrap_err();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("42703", "column \"missing\" does not exist"));
}

#[test]
fn test_select_reads_a_sequence_row() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE SEQUENCE sr", &[]).unwrap();
    let row = |conn: &mut Conn, sql: &str| -> String {
        let result = conn.simple_query_rows(sql).unwrap();
        result.rows[0].iter().map(|v| String::from_utf8(v.clone().unwrap()).unwrap()).collect::<Vec<_>>().join("|")
    };
    assert_eq!(row(&mut conn, "SELECT * FROM sr"), "1|0|f");
    conn.simple_query_rows("SELECT nextval('sr')").unwrap();
    assert_eq!(row(&mut conn, "SELECT *, tableoid > 0 FROM sr WHERE is_called"), "1|32|t|t");
}

#[test]
fn test_select_reads_rows_through_indexes() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE ix (id int PRIMARY KEY, k int NOT NULL, c text UNIQUE)", &[]).unwrap();
    conn.exec("CREATE INDEX ix_k ON ix (k)", &[]).unwrap();
    conn.exec("INSERT INTO ix VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 20, NULL), (4, 30, 'd'), (5, 40, 'e')", &[])
        .unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT * FROM ix WHERE id = 4"), ["4|30|d"]);
    assert_eq!(rows(&mut conn, "SELECT id FROM ix WHERE k = 20"), ["2", "3"]);
    assert_eq!(rows(&mut conn, "SELECT id, k FROM ix WHERE c = 'e'"), ["5|40"]);
    assert_eq!(rows(&mut conn, "SELECT id FROM ix WHERE c IS NULL"), ["3"]);
    assert_eq!(rows(&mut conn, "SELECT id FROM ix WHERE id > 3 AND id <= 5"), ["4", "5"]);
    conn.exec("UPDATE ix SET k = 50 WHERE id = 2", &[]).unwrap();
    conn.exec("DELETE FROM ix WHERE c = 'd'", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT id FROM ix WHERE k = 20"), ["3"]);
    assert_eq!(rows(&mut conn, "SELECT id, c FROM ix WHERE k = 50"), ["2|b"]);
    assert!(rows(&mut conn, "SELECT id FROM ix WHERE id = 4").is_empty());
}

#[test]
fn test_select_sorts_rows_by_order_by() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE so (id int PRIMARY KEY, k int NOT NULL, d float8)", &[]).unwrap();
    conn.exec("CREATE INDEX so_k ON so (k)", &[]).unwrap();
    conn.exec("INSERT INTO so VALUES (1, 3, 1.5), (2, 1, NULL), (3, 2, 2.5), (4, 1, 0.5), (5, 2, 1.5)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT id FROM so ORDER BY d DESC NULLS LAST, id"), ["3", "1", "5", "4", "2"]);
    assert_eq!(rows(&mut conn, "SELECT id, k FROM so ORDER BY k DESC, id DESC"), ["1|3", "5|2", "3|2", "4|1", "2|1"]);
    assert_eq!(rows(&mut conn, "SELECT k * 10 AS m FROM so WHERE id > 1 ORDER BY m, d"), ["10", "10", "20", "20"]);
    let result = conn.simple_query_rows("SELECT id FROM so ORDER BY d, k").unwrap();
    let fields: Vec<&str> = result.fields.iter().map(|f| f.name.as_str()).collect();
    assert_eq!(fields, ["id"]);
    assert_eq!(result.command_tag, "SELECT 5");
}

#[test]
fn test_select_keeps_the_rows_of_limit_and_offset() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE li (id int PRIMARY KEY, d float8)", &[]).unwrap();
    conn.exec("INSERT INTO li VALUES (1, 3), (2, 1), (3, 4), (4, 1), (5, 5), (6, 9), (7, 2)", &[]).unwrap();
    let ids = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        result.rows.iter().map(|row| String::from_utf8(row[0].clone().unwrap()).unwrap()).collect()
    };
    assert_eq!(ids(&mut conn, "SELECT id FROM li ORDER BY d DESC LIMIT 3"), ["6", "5", "3"]);
    let result = conn.simple_query_rows("SELECT id AS i FROM li ORDER BY id").unwrap();
    assert_eq!(result.fields[0].name, "i");
    assert_eq!(ids(&mut conn, "SELECT id FROM li ORDER BY id LIMIT 2 OFFSET 3"), ["4", "5"]);
    assert_eq!(ids(&mut conn, "SELECT id FROM li ORDER BY id DESC FETCH FIRST 2 ROWS ONLY"), ["7", "6"]);
    let result = conn.simple_query_rows("SELECT id FROM li LIMIT -1").unwrap();
    assert_eq!(result.fields[0].name, "id");
    let err = result.error.unwrap();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("2201W", "LIMIT must not be negative"));
}

#[test]
fn test_select_groups_rows_and_computes_aggregates() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE ag (id int PRIMARY KEY, k int NOT NULL, c text, d float8)", &[]).unwrap();
    conn.exec("CREATE INDEX ag_k ON ag (k)", &[]).unwrap();
    conn.exec(
        "INSERT INTO ag VALUES (1, 10, 'a', 1.5), (2, 20, 'b', NULL), (3, 20, NULL, 2.5), (4, 30, 'd', 0.5), \
         (5, 10, 'a', 3), (6, 40, 'b', 1), (7, 20, 'a', 2)",
        &[],
    )
    .unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT count(*), count(c), max(id), min(c), sum(k) FROM ag"), ["7|6|7|a|150"]);
    assert_eq!(rows(&mut conn, "SELECT k, count(*), sum(id) FROM ag GROUP BY k ORDER BY k"), [
        "10|2|6", "20|3|12", "30|1|4", "40|1|6"
    ]);
    assert_eq!(rows(&mut conn, "SELECT c, max(d) FROM ag GROUP BY c ORDER BY c"), [
        "a|3", "b|1", "d|0.5", "(null)|2.5"
    ]);
    assert_eq!(rows(&mut conn, "SELECT k FROM ag GROUP BY k HAVING count(*) > 1 ORDER BY k"), ["10", "20"]);
    assert_eq!(rows(&mut conn, "SELECT avg(k), stddev(d), bool_and(k > 5) FROM ag"), [
        "21.4285714285714286|0.9354143466934853|t"
    ]);
    assert_eq!(rows(&mut conn, "SELECT count(*) FILTER (WHERE c IS NOT NULL) FROM ag WHERE false"), ["0"]);
    assert_eq!(rows(&mut conn, "SELECT sum(k) FROM ag WHERE id BETWEEN 2 AND 5"), ["80"]);
    assert_eq!(rows(&mut conn, "SELECT sum(k::int8), avg(d::numeric), string_agg(c, ',') FROM ag"), [
        "150|1.7500000000000000|a,b,d,a,b,a"
    ]);
    assert_eq!(rows(&mut conn, "SELECT array_agg(id), jsonb_agg(c) FROM ag WHERE k = 20"), [
        "{2,3,7}|[\"b\", null, \"a\"]"
    ]);
    let sql = "SELECT k, count(DISTINCT c), string_agg(c, ',' ORDER BY id DESC) FROM ag GROUP BY k ORDER BY k";
    assert_eq!(rows(&mut conn, sql), ["10|1|a,a", "20|2|a,b", "30|1|d", "40|1|b"]);
}

#[test]
fn test_select_drops_repeated_rows_for_distinct() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE ds (id int PRIMARY KEY, k int NOT NULL, c text)", &[]).unwrap();
    let insert = "INSERT INTO ds VALUES (1, 3, 'c'), (2, 1, 'a'), (3, 2, 'b'), (4, 1, 'a'), (5, 2, NULL), (6, 3, 'c')";
    conn.exec(insert, &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT DISTINCT c FROM ds WHERE id BETWEEN 2 AND 6 ORDER BY c"), [
        "a", "b", "c", "(null)"
    ]);
    assert_eq!(rows(&mut conn, "SELECT DISTINCT k, c FROM ds ORDER BY k DESC, c"), [
        "3|c", "2|b", "2|(null)", "1|a"
    ]);
    assert_eq!(rows(&mut conn, "SELECT DISTINCT ON (k) k, id FROM ds ORDER BY k, id DESC"), ["1|4", "2|5", "3|6"]);
    assert_eq!(rows(&mut conn, "SELECT DISTINCT 1 FROM ds"), ["1"]);
}

#[test]
fn test_select_joins_tables() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let values = |count: i64, row: &dyn Fn(i64) -> String| (1..=count).map(row).collect::<Vec<_>>().join(", ");
    conn.exec("CREATE TABLE ja (id int PRIMARY KEY, x int, s text) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE jb (id int PRIMARY KEY, aid int, y text) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec(&format!("INSERT INTO ja VALUES {}", values(50, &|g| format!("({g}, {}, 'a{g}')", g % 7))), &[]).unwrap();
    conn.exec(&format!("INSERT INTO jb VALUES {}", values(80, &|g| format!("({g}, {}, 'b{g}')", g % 60))), &[])
        .unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT ja.s, jb.y FROM ja, jb WHERE ja.id = jb.aid AND ja.x = 3 ORDER BY jb.y"), [
        "a10|b10", "a17|b17", "a24|b24", "a3|b3", "a31|b31", "a38|b38", "a45|b45", "a3|b63", "a10|b70", "a17|b77"
    ]);
    assert_eq!(rows(&mut conn, "SELECT * FROM ja CROSS JOIN jb WHERE ja.id < 3 AND jb.id < 3"), [
        "1|1|a1|1|1|b1", "1|1|a1|2|2|b2", "2|2|a2|1|1|b1", "2|2|a2|2|2|b2"
    ]);
    let sql = "SELECT ja.id, jb.id, c.id FROM ja JOIN jb ON ja.id = jb.aid JOIN ja c ON c.x = jb.id % 7 \
               WHERE ja.id < 3 AND c.id < 10";
    assert_eq!(rows(&mut conn, sql), ["1|1|8", "1|1|1", "2|2|9", "2|2|2", "1|61|5", "2|62|6"]);
    assert_eq!(rows(&mut conn, "SELECT x, count(*) FROM ja JOIN jb ON ja.id = jb.aid GROUP BY x ORDER BY x"), [
        "0|9", "1|11", "2|10", "3|10", "4|10", "5|10", "6|10"
    ]);
    assert_eq!(rows(&mut conn, "SELECT count(*) FROM ja, jb WHERE ja.x < jb.id AND jb.id < 5"), ["73"]);
}

#[test]
fn test_select_merge_joins_tables() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE ma (id int PRIMARY KEY, k int, s text) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE mb (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE INDEX mb_k ON mb (k)", &[]).unwrap();
    conn.exec("INSERT INTO ma VALUES (1, 1, 'a1'), (2, 2, 'a2'), (3, 3, 'a3'), (4, 0, 'a4'), (5, 1, 'a5'), (6, 2, 'a6')", &[])
        .unwrap();
    conn.exec(
        "INSERT INTO mb VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 0), (6, 1), (7, 2), (8, 3), (9, NULL), (10, 0), \
         (11, 1), (12, 2)",
        &[],
    )
    .unwrap();
    conn.exec("SET enable_hashjoin = off", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT ma.s, mb.id FROM ma JOIN mb ON ma.k = mb.k"), [
        "a4|5", "a4|10", "a1|1", "a5|1", "a1|6", "a5|6", "a1|11", "a5|11", "a2|2", "a6|2", "a2|7", "a6|7", "a2|12",
        "a6|12", "a3|3", "a3|8"
    ]);
    assert_eq!(rows(&mut conn, "SELECT x.id, y.id FROM mb x JOIN mb y ON x.k = y.k"), [
        "5|5", "5|10", "10|5", "10|10", "1|1", "1|6", "1|11", "6|1", "6|6", "6|11", "11|1", "11|6", "11|11", "2|2",
        "2|7", "2|12", "7|2", "7|7", "7|12", "12|2", "12|7", "12|12", "3|3", "3|8", "8|3", "8|8", "4|4"
    ]);
}

#[test]
fn test_select_nested_loops_pass_outer_columns_to_index_scans() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE ma (id int PRIMARY KEY, k int, s text) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE mb (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE INDEX mb_k ON mb (k)", &[]).unwrap();
    conn.exec("INSERT INTO ma VALUES (1, 1, 'a1'), (2, 2, 'a2'), (3, 3, 'a3'), (4, 0, 'a4'), (5, 1, 'a5'), (6, 2, 'a6')", &[])
        .unwrap();
    conn.exec(
        "INSERT INTO mb VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 0), (6, 1), (7, 2), (8, 3), (9, NULL), (10, 0), \
         (11, 1), (12, 2)",
        &[],
    )
    .unwrap();
    conn.exec("SET enable_hashjoin = off", &[]).unwrap();
    conn.exec("SET enable_mergejoin = off", &[]).unwrap();
    let result = conn.simple_query_rows("SELECT ma.s, mb.id FROM ma JOIN mb ON ma.k = mb.k").unwrap();
    let text = |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
    let rows: Vec<String> =
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect();
    assert_eq!(rows, [
        "a1|1", "a1|6", "a1|11", "a2|2", "a2|7", "a2|12", "a3|3", "a3|8", "a4|5", "a4|10", "a5|1", "a5|6", "a5|11",
        "a6|2", "a6|7", "a6|12"
    ]);
}

#[test]
fn test_select_outer_joins_tables() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE oa (id int PRIMARY KEY, k int, s text) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE ob (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO oa VALUES (1, 1, 'a1'), (2, 2, 'a2'), (3, NULL, 'a3'), (4, 5, 'a4'), (5, 1, 'a5')", &[])
        .unwrap();
    conn.exec("INSERT INTO ob VALUES (1, 1), (2, 2), (3, 3), (4, NULL), (5, 2)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT oa.s, ob.id FROM oa LEFT JOIN ob ON oa.k = ob.k ORDER BY oa.id, ob.id"), [
        "a1|1", "a2|2", "a2|5", "a3|(null)", "a4|(null)", "a5|1"
    ]);
    assert_eq!(rows(&mut conn, "SELECT oa.s, ob.id FROM oa RIGHT JOIN ob ON oa.k = ob.k ORDER BY ob.id, oa.id"), [
        "a1|1", "a5|1", "a2|2", "(null)|3", "(null)|4", "a2|5"
    ]);
    let full = ["a1|1", "a2|2", "a2|5", "a3|(null)", "a4|(null)", "a5|1", "(null)|3", "(null)|4"];
    assert_eq!(rows(&mut conn, "SELECT oa.s, ob.id FROM oa FULL JOIN ob ON oa.k = ob.k ORDER BY oa.id, ob.id"), full);
    assert_eq!(rows(&mut conn, "SELECT oa.s FROM oa LEFT JOIN ob ON oa.k = ob.k WHERE ob.k IS NULL ORDER BY oa.id"), [
        "a3", "a4"
    ]);
    conn.exec("SET enable_hashjoin = off", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT oa.s, ob.id FROM oa FULL JOIN ob ON oa.k = ob.k ORDER BY oa.id, ob.id"), full);
}

#[test]
fn test_select_pulled_up_subqueries() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE sa (id int PRIMARY KEY, x int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE sb (id int PRIMARY KEY, aid int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO sa VALUES (1, 10), (2, NULL), (3, 30)", &[]).unwrap();
    conn.exec("INSERT INTO sb VALUES (1, 1), (2, 2), (3, 7)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT s.v FROM (SELECT x + 1 AS v FROM sa WHERE id > 1) s ORDER BY s.v"), [
        "31", "(null)"
    ]);
    assert_eq!(
        rows(&mut conn, "SELECT sb.id, s.one, s.cx FROM sb LEFT JOIN (SELECT id, 1 AS one, coalesce(x, 0) AS cx FROM sa) s \
                         ON s.id = sb.aid ORDER BY sb.id"),
        ["1|1|10", "2|1|0", "3|(null)|(null)"]
    );
    assert_eq!(rows(&mut conn, "SELECT * FROM (VALUES (1, 'one')) v(a, b)"), ["1|one"]);
}

#[test]
fn test_select_subquery_scans() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE sc (id int PRIMARY KEY, x int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE sd (id int PRIMARY KEY, y int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO sc VALUES (1, 10), (2, NULL), (3, 10), (4, 1)", &[]).unwrap();
    conn.exec("INSERT INTO sd VALUES (1, 10), (2, 0), (3, 1)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT * FROM (SELECT x, count(*) AS c FROM sc GROUP BY x) s WHERE s.c > 1"), ["10|2"]);
    assert_eq!(rows(&mut conn, "SELECT * FROM (SELECT id, x FROM sc ORDER BY id LIMIT 2) s WHERE s.x > 3"), ["1|10"]);
    assert_eq!(
        rows(&mut conn, "SELECT sd.id, s.c FROM sd LEFT JOIN (SELECT x, count(*) AS c FROM sc GROUP BY x) s ON s.x = sd.y \
                         ORDER BY sd.id"),
        ["1|2", "2|(null)", "3|1"]
    );
}

#[test]
fn test_select_sublinks() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE sl1 (id int PRIMARY KEY, x int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE sl2 (id int PRIMARY KEY, aid int, y int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO sl1 VALUES (1, 10), (2, NULL), (3, 30)", &[]).unwrap();
    conn.exec("INSERT INTO sl2 VALUES (1, 1, 10), (2, 3, 0), (3, NULL, 5)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT id, (SELECT max(x) FROM sl1) FROM sl2 ORDER BY id"), ["1|30", "2|30", "3|30"]);
    assert_eq!(
        rows(&mut conn, "SELECT sl2.id, (SELECT x FROM sl1 WHERE sl1.id = sl2.aid) FROM sl2 ORDER BY 1"),
        ["1|10", "2|30", "3|(null)"]
    );
    assert_eq!(
        rows(&mut conn, "SELECT id, aid NOT IN (SELECT x FROM sl1), y IN (SELECT x FROM sl1 WHERE x IS NOT NULL) FROM sl2 \
                         ORDER BY id"),
        ["1|(null)|t", "2|(null)|f", "3|(null)|f"]
    );
    assert_eq!(rows(&mut conn, "SELECT ARRAY(SELECT x FROM sl1 ORDER BY id)"), ["{10,NULL,30}"]);
    assert_eq!(
        rows(&mut conn, "UPDATE sl2 SET (aid, y) = (SELECT id, x FROM sl1 WHERE sl1.id = sl2.id) WHERE id < 3 RETURNING *"),
        ["1|1|10", "2|2|(null)"]
    );
}

#[test]
fn test_select_semi_joins() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE sj1 (id int PRIMARY KEY, x int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE sj2 (id int PRIMARY KEY, aid int, y int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO sj1 VALUES (1, 10), (2, NULL), (3, 30)", &[]).unwrap();
    conn.exec("INSERT INTO sj2 VALUES (1, 1, 10), (2, 3, 0), (3, NULL, 10), (4, 7, 30)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT id FROM sj2 WHERE aid IN (SELECT id FROM sj1) ORDER BY id"), ["1", "2"]);
    assert_eq!(rows(&mut conn, "SELECT id FROM sj2 WHERE y IN (SELECT x FROM sj1) ORDER BY id"), ["1", "3", "4"]);
    assert_eq!(
        rows(&mut conn, "SELECT id FROM sj2 WHERE NOT EXISTS (SELECT 1 FROM sj1 WHERE sj1.id = sj2.aid) ORDER BY id"),
        ["3", "4"]
    );
    assert_eq!(rows(&mut conn, "SELECT id FROM sj2 WHERE aid IN (VALUES (1), (7)) ORDER BY id"), ["1", "4"]);
}

#[test]
fn test_select_lateral() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE lt1 (id int PRIMARY KEY, x int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE TABLE lt2 (id int PRIMARY KEY, aid int, y int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO lt1 VALUES (1, 10), (2, NULL), (3, 30)", &[]).unwrap();
    conn.exec("INSERT INTO lt2 VALUES (1, 1, 10), (2, 3, 0), (3, NULL, 10), (4, 1, 30)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(
        rows(&mut conn, "SELECT lt1.id, ss.y FROM lt1, LATERAL (SELECT y FROM lt2 WHERE lt2.aid = lt1.id OFFSET 0) ss ORDER BY 1, 2"),
        ["1|10", "1|30", "3|0"]
    );
    assert_eq!(
        rows(&mut conn, "SELECT lt1.id, c.n FROM lt1, LATERAL (SELECT count(*) AS n FROM lt2 WHERE lt2.aid = lt1.id) c ORDER BY 1"),
        ["1|2", "2|0", "3|1"]
    );
    assert_eq!(
        rows(&mut conn, "SELECT lt1.id, v.a FROM lt1, LATERAL (VALUES (lt1.x), (lt1.id)) v(a) ORDER BY 1, 2"),
        ["1|1", "1|10", "2|2", "2|(null)", "3|3", "3|30"]
    );
    assert_eq!(
        rows(&mut conn, "SELECT lt1.id, ss.lx FROM lt1 LEFT JOIN LATERAL (SELECT lt1.x AS lx FROM lt2 WHERE lt2.aid = lt1.id) ss ON TRUE ORDER BY 1, 2"),
        ["1|10", "1|10", "2|(null)", "3|30"]
    );
    assert_eq!(rows(&mut conn, "SELECT id FROM lt1 WHERE x IN (SELECT y FROM lt2 WHERE lt2.aid = lt1.id) ORDER BY id"), ["1"]);
}

#[test]
fn test_select_minmax_and_join_or_conditions() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE mx1 (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("CREATE INDEX mx1_k ON mx1 (k)", &[]).unwrap();
    conn.exec("CREATE TABLE mx2 (id int PRIMARY KEY, aid int, y int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO mx1 VALUES (1, 5), (2, NULL), (3, 2), (4, 9)", &[]).unwrap();
    conn.exec("INSERT INTO mx2 VALUES (1, 1, 2), (2, 3, 4), (3, 4, 9)", &[]).unwrap();
    let rows = |conn: &mut Conn, sql: &str| -> Vec<String> {
        let result = conn.simple_query_rows(sql).unwrap();
        let text =
            |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".to_string(), |v| String::from_utf8(v.clone()).unwrap());
        result.rows.iter().map(|row| row.iter().map(text).collect::<Vec<_>>().join("|")).collect()
    };
    assert_eq!(rows(&mut conn, "SELECT min(k), max(k) FROM mx1"), ["2|9"]);
    assert_eq!(rows(&mut conn, "SELECT max(k) FROM mx1 WHERE k < 9 HAVING max(k) > 1"), ["5"]);
    assert_eq!(rows(&mut conn, "SELECT max(id) FROM mx1 WHERE k IS NULL"), ["2"]);
    assert_eq!(
        rows(&mut conn, "SELECT mx1.id, mx2.id FROM mx1 JOIN mx2 ON mx1.id = mx2.aid WHERE (mx1.k = 5 AND mx2.y = 2) OR (mx1.k = 2 AND mx2.y = 4) ORDER BY 1"),
        ["1|1", "3|2"]
    );
}

#[test]
fn test_select_sql_function_inlining() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE fn1 (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false)", &[]).unwrap();
    conn.exec("INSERT INTO fn1 VALUES (1, 3), (2, NULL)", &[]).unwrap();
    conn.exec("CREATE FUNCTION fn_add(int, int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 + $2'", &[]).unwrap();
    conn.exec("SET check_function_bodies = off", &[]).unwrap();
    conn.exec("CREATE FUNCTION fn_bad(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 +'", &[]).unwrap();
    let result = conn.simple_query_rows("SELECT id, fn_add(k, id) FROM fn1 ORDER BY id").unwrap();
    assert_eq!(result.rows, [vec![Some(b"1".to_vec()), Some(b"4".to_vec())], vec![Some(b"2".to_vec()), None]]);
    let err = conn.simple_query_rows("SELECT fn_bad(k) FROM fn1").unwrap_err();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("42601", "syntax error at end of input"));
    assert_eq!((fields.position, fields.internal_position, fields.internal_query.as_str()), (0, 12, "SELECT $1 +"));
    assert_eq!(fields.where_, "SQL function \"fn_bad\" during inlining");
}
