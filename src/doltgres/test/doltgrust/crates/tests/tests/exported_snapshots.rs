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

//! Tests of snapshots one session exports with `pg_export_snapshot` and another imports with `SET TRANSACTION
//! SNAPSHOT`.

use harness::conn::connect;
use harness::pgx::Conn;
use harness::server::{Server, Target};

/// value runs a query that returns one value and returns it as text.
fn value(conn: &mut Conn, sql: &str) -> String {
    let result = conn.simple_query_rows(sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    String::from_utf8(result.rows[0][0].clone().unwrap()).unwrap()
}

/// fails runs a statement that fails and returns its SQLSTATE and message.
fn fails(conn: &mut Conn, sql: &str) -> (String, String) {
    let err = conn.exec(sql, &[]).expect_err(sql);
    let fields = &err.pg_error().unwrap_or_else(|| panic!("{sql}: {err}")).fields;
    (fields.code.clone(), fields.message.clone())
}

/// exec runs each statement, panicking on an error.
fn exec(conn: &mut Conn, statements: &[&str]) {
    for sql in statements {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
}

#[test]
fn test_an_imported_snapshot_sees_what_the_exporting_transaction_saw() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    exec(&mut setup, &["CREATE DATABASE snapshots_other", "CREATE TABLE t (i int)", "INSERT INTO t VALUES (1)"]);
    let mut a = connect(&server, "postgres", "postgres");
    let mut b = connect(&server, "postgres", "postgres");
    let mut other = connect(&server, "snapshots_other", "postgres");
    exec(&mut a, &["BEGIN ISOLATION LEVEL REPEATABLE READ", "INSERT INTO t VALUES (100)"]);
    let id = value(&mut a, "SELECT pg_export_snapshot()");
    exec(&mut setup, &["INSERT INTO t VALUES (2)"]);
    let set = format!("SET TRANSACTION SNAPSHOT '{id}'");
    exec(&mut b, &["BEGIN ISOLATION LEVEL REPEATABLE READ", &set]);
    assert_eq!(value(&mut b, "SELECT array_agg(i ORDER BY i) FROM t"), "{1}");
    exec(&mut b, &["INSERT INTO t VALUES (5)"]);
    assert_eq!(value(&mut b, "SELECT array_agg(i ORDER BY i) FROM t"), "{1,5}");
    exec(&mut b, &["ROLLBACK", "BEGIN ISOLATION LEVEL SERIALIZABLE"]);
    let refused = (
        "0A000".to_string(),
        "a serializable transaction cannot import a snapshot from a non-serializable transaction".to_string(),
    );
    assert_eq!(fails(&mut b, &set), refused);
    exec(&mut b, &["ROLLBACK"]);
    exec(&mut other, &["BEGIN ISOLATION LEVEL REPEATABLE READ"]);
    let refused = ("0A000".to_string(), "cannot import a snapshot from a different database".to_string());
    assert_eq!(fails(&mut other, &set), refused);
    exec(&mut a, &["SAVEPOINT s"]);
    let refused = ("25001".to_string(), "cannot export a snapshot from a subtransaction".to_string());
    assert_eq!(fails(&mut a, "SELECT pg_export_snapshot()"), refused);
    exec(&mut a, &["ROLLBACK TO s", "COMMIT"]);
    exec(&mut b, &["BEGIN ISOLATION LEVEL REPEATABLE READ"]);
    assert_eq!(fails(&mut b, &set), ("42704".to_string(), format!("snapshot \"{id}\" does not exist")));
    exec(&mut b, &["ROLLBACK"]);

    exec(&mut a, &["BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"]);
    let set = format!("SET TRANSACTION SNAPSHOT '{}'", value(&mut a, "SELECT pg_export_snapshot()"));
    exec(&mut b, &["BEGIN ISOLATION LEVEL SERIALIZABLE"]);
    let refused = (
        "0A000".to_string(),
        "a non-read-only serializable transaction cannot import a snapshot from a read-only transaction".to_string(),
    );
    assert_eq!(fails(&mut b, &set), refused);
    exec(&mut b, &["ROLLBACK", "BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY", &set]);
    exec(&mut setup, &["INSERT INTO t VALUES (3)"]);
    assert_eq!(value(&mut b, "SELECT array_agg(i ORDER BY i) FROM t"), "{1,2,100}");
    exec(&mut b, &["COMMIT"]);
    exec(&mut a, &["COMMIT", "BEGIN"]);
    value(&mut a, "SELECT pg_export_snapshot()");
    let refused = ("0A000".to_string(), "cannot PREPARE a transaction that has exported snapshots".to_string());
    assert_eq!(fails(&mut a, "PREPARE TRANSACTION 'p'"), refused);
    exec(&mut a, &["ROLLBACK"]);
}
