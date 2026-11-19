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
//! Tests of INSERT, UPDATE, DELETE, and TRUNCATE over the simple query protocol.

use harness::conn::{connect, error};
use harness::pgx::Conn;
use harness::server::{Server, Target};

/// rows returns a query's rows, each as its values joined by `|`, NULL as `(null)`.
fn rows(conn: &mut Conn, sql: &str) -> Vec<String> {
    let result = conn.simple_query_rows(sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    result
        .rows
        .iter()
        .map(|row| {
            let value =
                |v: &Option<Vec<u8>>| v.as_ref().map_or("(null)".into(), |v| String::from_utf8(v.clone()).unwrap());
            row.iter().map(value).collect::<Vec<String>>().join("|")
        })
        .collect()
}

#[test]
fn test_insert_adds_rows_checked_against_the_tables_constraints() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let sql = "CREATE TABLE it (id serial PRIMARY KEY, name text NOT NULL, n int DEFAULT 7 CHECK (n > 0))";
    conn.exec(sql, &[]).unwrap();
    assert_eq!(conn.exec("INSERT INTO it (name) VALUES ('a')", &[]).unwrap(), "INSERT 0 1");
    let result = conn.simple_query_rows("INSERT INTO it (name, n) VALUES ('b', 2) RETURNING *, n * 10").unwrap();
    assert_eq!(result.command_tag, "INSERT 0 1");
    assert_eq!(rows(&mut conn, "INSERT INTO it (name, n) VALUES ('c', 3) RETURNING id, name"), ["3|c"]);
    let s = |v: &str| v.to_string();
    assert_eq!(
        error(&mut conn, "INSERT INTO it (name, n) VALUES (NULL, 3)"),
        (
            s("23502"),
            s("null value in column \"name\" of relation \"it\" violates not-null constraint"),
            s("Failing row contains (4, null, 3).")
        )
    );
    assert_eq!(
        error(&mut conn, "INSERT INTO it (name, n) VALUES ('d', -1)"),
        (
            s("23514"),
            s("new row for relation \"it\" violates check constraint \"it_n_check\""),
            s("Failing row contains (5, d, -1).")
        )
    );
    assert_eq!(
        error(&mut conn, "INSERT INTO it VALUES (1, 'dup')"),
        (s("23505"), s("duplicate key value violates unique constraint \"it_pkey\""), s("Key (id)=(1) already exists."))
    );
    assert_eq!(rows(&mut conn, "SELECT * FROM it"), ["1|a|7", "2|b|2", "3|c|3"]);
    conn.exec("BEGIN READ ONLY", &[]).unwrap();
    assert_eq!(
        error(&mut conn, "INSERT INTO it (name) VALUES ('e')"),
        (s("25006"), s("cannot execute INSERT in a read-only transaction"), s(""))
    );
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("INSERT INTO it (name) VALUES ('f')", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT name FROM it WHERE name = 'f'"), ["f"]);
    conn.exec("ROLLBACK", &[]).unwrap();
    assert!(rows(&mut conn, "SELECT name FROM it WHERE name = 'f'").is_empty());
    let long = "x".repeat(70);
    let (code, _, detail) = error(&mut conn, &format!("INSERT INTO it (name, n) VALUES ('{long}', 0)"));
    assert_eq!((code.as_str(), detail), ("23514", format!("Failing row contains (7, {}..., 0).", "x".repeat(64))));
}

#[test]
fn test_insert_adds_the_rows_of_a_values_list() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE TABLE iv (a int PRIMARY KEY, b text)", &[]).unwrap();
    assert_eq!(conn.exec("INSERT INTO iv VALUES (1, 'a'), (2, NULL), (3, 'c' || 'd')", &[]).unwrap(), "INSERT 0 3");
    assert_eq!(rows(&mut conn, "INSERT INTO iv (b, a) VALUES ('x', 5), ('y', 4) RETURNING a"), ["5", "4"]);
    let mut all = rows(&mut conn, "SELECT * FROM iv");
    all.sort();
    assert_eq!(all, ["1|a", "2|(null)", "3|cd", "4|y", "5|x"]);
    let (code, message, detail) = error(&mut conn, "INSERT INTO iv VALUES (6, 'f'), (1, 'dup')");
    assert_eq!(
        (code.as_str(), message.as_str(), detail.as_str()),
        ("23505", "duplicate key value violates unique constraint \"iv_pkey\"", "Key (a)=(1) already exists.")
    );
    assert!(rows(&mut conn, "SELECT a FROM iv WHERE a = 6").is_empty());
}

#[test]
fn test_update_and_delete_change_the_rows_they_find() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let sorted = |conn: &mut Conn, sql: &str| {
        let mut all = rows(conn, sql);
        all.sort();
        all
    };
    conn.exec("CREATE TABLE ut (id int PRIMARY KEY, name text NOT NULL, n int CHECK (n > 0))", &[]).unwrap();
    conn.exec("INSERT INTO ut VALUES (1, 'a', 1), (2, 'b', 2), (3, 'c', 3)", &[]).unwrap();
    assert_eq!(conn.exec("UPDATE ut SET n = n + 10 WHERE id > 1", &[]).unwrap(), "UPDATE 2");
    assert_eq!(rows(&mut conn, "UPDATE ut SET name = upper(name) WHERE id = 3 RETURNING *"), ["3|C|13"]);
    let (code, message, detail) = error(&mut conn, "UPDATE ut SET n = -1 WHERE id = 1");
    assert_eq!(
        (code.as_str(), message.as_str(), detail.as_str()),
        ("23514", "new row for relation \"ut\" violates check constraint \"ut_n_check\"", "Failing row contains (1, a, -1).")
    );
    let (code, message, _) = error(&mut conn, "UPDATE ut SET name = NULL WHERE id = 2");
    assert_eq!(
        (code.as_str(), message.as_str()),
        ("23502", "null value in column \"name\" of relation \"ut\" violates not-null constraint")
    );
    assert_eq!(conn.exec("UPDATE ut SET id = 10 WHERE id = 1", &[]).unwrap(), "UPDATE 1");
    let (code, _, detail) = error(&mut conn, "UPDATE ut SET id = 2 WHERE id = 10");
    assert_eq!((code.as_str(), detail.as_str()), ("23505", "Key (id)=(2) already exists."));
    assert_eq!(rows(&mut conn, "DELETE FROM ut WHERE n > 12 RETURNING id, name"), ["3|C"]);
    assert_eq!(conn.exec("DELETE FROM ut WHERE id = 99", &[]).unwrap(), "DELETE 0");
    assert_eq!(sorted(&mut conn, "SELECT * FROM ut"), ["10|a|1", "2|b|12"]);
    conn.exec("CREATE TABLE ukl (a int, b text)", &[]).unwrap();
    conn.exec("INSERT INTO ukl VALUES (1, 'x'), (1, 'x'), (2, 'y')", &[]).unwrap();
    assert_eq!(conn.exec("UPDATE ukl SET b = 'z' WHERE a = 1", &[]).unwrap(), "UPDATE 2");
    assert_eq!(sorted(&mut conn, "SELECT * FROM ukl"), ["1|z", "1|z", "2|y"]);
    assert_eq!(conn.exec("DELETE FROM ukl WHERE b = 'z'", &[]).unwrap(), "DELETE 2");
    assert_eq!(sorted(&mut conn, "SELECT * FROM ukl"), ["2|y"]);
    conn.exec("BEGIN", &[]).unwrap();
    assert_eq!(conn.exec("DELETE FROM ukl", &[]).unwrap(), "DELETE 1");
    conn.exec("ROLLBACK", &[]).unwrap();
    assert_eq!(sorted(&mut conn, "SELECT * FROM ukl"), ["2|y"]);
}

#[test]
fn test_update_of_a_row_a_concurrent_transaction_changed_fails_under_repeatable_read() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut a = connect(&server, "postgres", "postgres");
    let mut b = connect(&server, "postgres", "postgres");
    a.exec("CREATE TABLE cu (id int PRIMARY KEY, n int)", &[]).unwrap();
    a.exec("INSERT INTO cu VALUES (1, 0), (2, 0)", &[]).unwrap();
    for (change, message) in [
        ("UPDATE cu SET n = n + 1 WHERE id = 1", "could not serialize access due to concurrent update"),
        ("DELETE FROM cu WHERE id = 2", "could not serialize access due to concurrent delete"),
    ] {
        a.exec("BEGIN ISOLATION LEVEL REPEATABLE READ", &[]).unwrap();
        a.exec("SELECT * FROM cu", &[]).unwrap();
        b.exec(change, &[]).unwrap();
        let (code, got, _) = error(&mut a, "UPDATE cu SET n = 5");
        assert_eq!((code.as_str(), got.as_str()), ("40001", message), "{change}");
        a.exec("ROLLBACK", &[]).unwrap();
    }
}

#[test]
fn test_truncate_empties_tables_as_of_the_transactions_commit() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let sorted = |conn: &mut Conn, sql: &str| {
        let mut all = rows(conn, sql);
        all.sort();
        all
    };
    conn.exec("CREATE TABLE tt (id SERIAL PRIMARY KEY, k INT UNIQUE, v TEXT)", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (10, 'a'), (20, 'b')", &[]).unwrap();
    assert_eq!(conn.exec("TRUNCATE tt", &[]).unwrap(), "TRUNCATE TABLE");
    assert!(rows(&mut conn, "SELECT * FROM tt").is_empty());
    conn.exec("INSERT INTO tt (k, v) VALUES (10, 'c')", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT * FROM tt WHERE k = 10"), ["3|10|c"]);
    conn.exec("TRUNCATE tt RESTART IDENTITY", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (10, 'd')", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT * FROM tt"), ["1|10|d"]);
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("TRUNCATE tt RESTART IDENTITY", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (20, 'e')", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT * FROM tt"), ["1|20|e"]);
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (30, 'f')", &[]).unwrap();
    assert_eq!(sorted(&mut conn, "SELECT * FROM tt"), ["1|10|d", "2|30|f"]);
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (40, 'g')", &[]).unwrap();
    conn.exec("SAVEPOINT s", &[]).unwrap();
    conn.exec("TRUNCATE tt", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (50, 'h')", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT k FROM tt"), ["50"]);
    conn.exec("ROLLBACK TO s", &[]).unwrap();
    assert_eq!(sorted(&mut conn, "SELECT k FROM tt"), ["10", "30", "40"]);
    conn.exec("TRUNCATE TABLE tt, tt", &[]).unwrap();
    conn.exec("INSERT INTO tt (k, v) VALUES (60, 'i')", &[]).unwrap();
    conn.exec("COMMIT", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT k FROM tt"), ["60"]);
    conn.exec("CREATE VIEW tv AS SELECT 1", &[]).unwrap();
    conn.exec("CREATE SEQUENCE ts", &[]).unwrap();
    for (sql, expected) in [
        ("TRUNCATE tv", ("42809", "\"tv\" is not a table")),
        ("TRUNCATE ts", ("42809", "\"ts\" is not a table")),
        ("TRUNCATE missing", ("42P01", "relation \"missing\" does not exist")),
        ("TRUNCATE pg_class", ("42501", "permission denied: \"pg_class\" is a system catalog")),
    ] {
        let (code, message, _) = error(&mut conn, sql);
        assert_eq!((code.as_str(), message.as_str()), expected, "{sql}");
    }
    conn.exec("CREATE TABLE tp (id INT PRIMARY KEY)", &[]).unwrap();
    conn.exec("CREATE TABLE tc (id INT REFERENCES tp)", &[]).unwrap();
    let (code, message, detail) = error(&mut conn, "TRUNCATE tp");
    assert_eq!(
        (code.as_str(), message.as_str(), detail.as_str()),
        ("0A000", "cannot truncate a table referenced in a foreign key constraint", "Table \"tc\" references \"tp\".")
    );
    conn.exec("TRUNCATE tp, tc", &[]).unwrap();
    conn.take_notices();
    conn.exec("TRUNCATE tp CASCADE", &[]).unwrap();
    let notices: Vec<String> = conn.take_notices().into_iter().map(|n| n.message).collect();
    assert_eq!(notices, ["truncate cascades to table \"tc\""]);
    conn.exec("BEGIN READ ONLY", &[]).unwrap();
    let (code, message, _) = error(&mut conn, "TRUNCATE tt");
    assert_eq!(
        (code.as_str(), message.as_str()),
        ("25006", "cannot execute TRUNCATE TABLE in a read-only transaction")
    );
    conn.exec("ROLLBACK", &[]).unwrap();
}

#[test]
fn test_copy_freeze_needs_a_table_created_or_truncated_in_the_subtransaction() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let not_here =
        "cannot perform COPY FREEZE because the table was not created or truncated in the current subtransaction";
    let copy_error = |conn: &mut Conn, sql: &str| {
        let err = conn.copy_from(sql, b"9\n").expect_err(sql);
        let fields = &err.pg_error().unwrap_or_else(|| panic!("{sql}: {err}")).fields;
        (fields.code.clone(), fields.message.clone())
    };
    conn.exec("CREATE TABLE cf (a INT)", &[]).unwrap();
    let (code, message) = copy_error(&mut conn, "COPY cf FROM STDIN WITH (FREEZE)");
    assert_eq!((code.as_str(), message.as_str()), ("55000", not_here));
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("TRUNCATE cf", &[]).unwrap();
    assert_eq!(conn.copy_from("COPY cf FROM STDIN WITH (FREEZE)", b"1\n").unwrap(), "COPY 1");
    conn.exec("COMMIT", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("CREATE TABLE cg (a INT)", &[]).unwrap();
    assert_eq!(conn.copy_from("COPY cg FROM STDIN WITH (FREEZE)", b"2\n").unwrap(), "COPY 1");
    conn.exec("COMMIT", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("TRUNCATE cf", &[]).unwrap();
    conn.exec("SAVEPOINT s", &[]).unwrap();
    let (code, message) = copy_error(&mut conn, "COPY cf FROM STDIN WITH (FREEZE)");
    assert_eq!((code.as_str(), message.as_str()), ("55000", not_here));
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("SAVEPOINT s", &[]).unwrap();
    conn.exec("TRUNCATE cf", &[]).unwrap();
    conn.exec("RELEASE s", &[]).unwrap();
    assert_eq!(conn.copy_from("COPY cf FROM STDIN WITH (FREEZE)", b"4\n").unwrap(), "COPY 1");
    conn.exec("COMMIT", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("CREATE INDEX ON cf (a)", &[]).unwrap();
    let (code, message) = copy_error(&mut conn, "COPY cf FROM STDIN WITH (FREEZE)");
    assert_eq!((code.as_str(), message.as_str()), ("55000", not_here));
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("CREATE TABLE ch (a INT)", &[]).unwrap();
    conn.exec("SAVEPOINT s", &[]).unwrap();
    let (code, message) = copy_error(&mut conn, "COPY ch FROM STDIN WITH (FREEZE)");
    assert_eq!((code.as_str(), message.as_str()), ("55000", not_here));
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.exec("CREATE TABLE cp (a INT) PARTITION BY RANGE (a)", &[]).unwrap();
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("TRUNCATE cp", &[]).unwrap();
    let (code, message) = copy_error(&mut conn, "COPY cp FROM STDIN WITH (FREEZE)");
    assert_eq!((code.as_str(), message.as_str()), ("0A000", "cannot perform COPY FREEZE on a partitioned table"));
    conn.exec("ROLLBACK", &[]).unwrap();
    assert_eq!(rows(&mut conn, "SELECT * FROM cf"), ["4"]);
    assert_eq!(rows(&mut conn, "SELECT * FROM cg"), ["2"]);
}

#[test]
fn test_select_for_update_locks_the_rows_it_returns() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut a = connect(&server, "postgres", "postgres");
    let mut b = connect(&server, "postgres", "postgres");
    a.exec("CREATE TABLE lk (id INT PRIMARY KEY, v TEXT)", &[]).unwrap();
    a.exec("INSERT INTO lk VALUES (1, 'a'), (2, 'b'), (3, 'c')", &[]).unwrap();
    a.exec("BEGIN", &[]).unwrap();
    assert_eq!(rows(&mut a, "SELECT * FROM lk WHERE id = 1 FOR UPDATE"), ["1|a"]);
    let (code, message, _) = error(&mut b, "SELECT * FROM lk WHERE id = 1 FOR UPDATE NOWAIT");
    assert_eq!((code.as_str(), message.as_str()), ("55P03", "could not obtain lock on row in relation \"lk\""));
    assert_eq!(rows(&mut b, "SELECT * FROM lk ORDER BY id FOR UPDATE SKIP LOCKED"), ["2|b", "3|c"]);
    let (code, message, _) = error(&mut b, "SELECT * FROM lk WHERE id = 1 FOR KEY SHARE NOWAIT");
    assert_eq!((code.as_str(), message.as_str()), ("55P03", "could not obtain lock on row in relation \"lk\""));
    a.exec("ROLLBACK", &[]).unwrap();
    a.exec("BEGIN", &[]).unwrap();
    assert_eq!(rows(&mut a, "SELECT * FROM lk WHERE id = 1 FOR NO KEY UPDATE"), ["1|a"]);
    assert_eq!(rows(&mut b, "SELECT * FROM lk WHERE id = 1 FOR KEY SHARE NOWAIT"), ["1|a"]);
    b.exec("SET lock_timeout = '50ms'", &[]).unwrap();
    let (code, message, _) = error(&mut b, "SELECT * FROM lk WHERE id = 1 FOR SHARE");
    assert_eq!((code.as_str(), message.as_str()), ("55P03", "canceling statement due to lock timeout"));
    b.exec("RESET lock_timeout", &[]).unwrap();
    a.exec("COMMIT", &[]).unwrap();
    assert_eq!(rows(&mut b, "SELECT * FROM lk WHERE id = 1 FOR SHARE NOWAIT"), ["1|a"]);
    a.exec("BEGIN ISOLATION LEVEL REPEATABLE READ", &[]).unwrap();
    assert_eq!(rows(&mut a, "SELECT v FROM lk WHERE id = 2"), ["b"]);
    b.exec("UPDATE lk SET v = 'x' WHERE id = 2", &[]).unwrap();
    let (code, message, _) = error(&mut a, "SELECT * FROM lk WHERE id = 2 FOR UPDATE");
    assert_eq!((code.as_str(), message.as_str()), ("40001", "could not serialize access due to concurrent update"));
    a.exec("ROLLBACK", &[]).unwrap();
}

#[test]
fn test_read_committed_rechecks_rows_a_concurrent_transaction_changed() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut a = connect(&server, "postgres", "postgres");
    let blocked = |a: &mut Conn, b: Conn, sql: &'static str| {
        let waiting = std::thread::spawn(move || {
            let mut b = b;
            let result = rows(&mut b, sql);
            (b, result)
        });
        std::thread::sleep(std::time::Duration::from_millis(300));
        a.exec("COMMIT", &[]).unwrap();
        waiting.join().unwrap()
    };
    a.exec("CREATE TABLE eq (id INT PRIMARY KEY, v INT, w INT)", &[]).unwrap();
    a.exec("CREATE INDEX eq_v ON eq (v)", &[]).unwrap();
    a.exec("CREATE TABLE eo (id INT PRIMARY KEY, x INT)", &[]).unwrap();
    a.exec("INSERT INTO eq VALUES (1, 1, 0), (2, 2, 0), (3, 3, 0)", &[]).unwrap();
    a.exec("INSERT INTO eo VALUES (1, 7), (2, 8), (3, 9)", &[]).unwrap();
    let b = connect(&server, "postgres", "postgres");
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = 10 WHERE id = 1", &[]).unwrap();
    a.exec("UPDATE eq SET v = 20 WHERE id = 2", &[]).unwrap();
    let (b, result) = blocked(&mut a, b, "SELECT * FROM eq WHERE v < 15 ORDER BY id FOR UPDATE");
    assert_eq!(result, ["1|10|0", "3|3|0"]);
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = v + 1 WHERE id = 1", &[]).unwrap();
    a.exec("DELETE FROM eq WHERE id = 3", &[]).unwrap();
    let (b, mut result) = blocked(&mut a, b, "UPDATE eq SET w = v * 10 WHERE v > 5 RETURNING *");
    result.sort();
    assert_eq!(result, ["1|11|110", "2|20|200"]);
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = 0 WHERE id = 1", &[]).unwrap();
    let (b, result) = blocked(&mut a, b, "DELETE FROM eq WHERE v > 5 RETURNING *");
    assert_eq!(result, ["2|20|200"]);
    a.exec("INSERT INTO eq VALUES (3, 3, 0)", &[]).unwrap();
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = 100 WHERE id = 1", &[]).unwrap();
    let (b, mut result) =
        blocked(&mut a, b, "UPDATE eq SET w = eo.x FROM eo WHERE eo.id = eq.id AND eq.v >= 0 RETURNING eq.*, eo.x");
    result.sort();
    assert_eq!(result, ["1|100|7|7", "3|3|9|9"]);
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = 5 WHERE id = 3", &[]).unwrap();
    let (b, result) =
        blocked(&mut a, b, "SELECT eq.id, eq.v, eo.x FROM eq JOIN eo ON eo.id = eq.id WHERE eq.v < 4 FOR UPDATE OF eq");
    assert!(result.is_empty());
    a.exec("BEGIN", &[]).unwrap();
    a.exec("UPDATE eq SET v = 7 WHERE id = 3", &[]).unwrap();
    let (mut b, result) = blocked(&mut a, b, "UPDATE eq SET v = v + 1 WHERE id = 3 RETURNING *");
    assert_eq!(result, ["3|8|9"]);
    b.exec("SET enable_seqscan = off", &[]).unwrap();
    assert!(rows(&mut b, "SELECT * FROM eq WHERE v = 7").is_empty());
    assert!(rows(&mut b, "SELECT * FROM eq WHERE v = 5").is_empty());
    assert_eq!(rows(&mut b, "SELECT * FROM eq WHERE v = 8"), ["3|8|9"]);
}
