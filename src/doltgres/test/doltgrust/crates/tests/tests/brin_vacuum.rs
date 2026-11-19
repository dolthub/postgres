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

//! Tests that VACUUM summarizes a BRIN index's ranges without waiting for the transactions writing its table. Postgres
//! summarizes them while a transaction writes the table, and Doltgrust leaves them to the next VACUUM, since a summary
//! has to hold the rows of writers it cannot see.

use std::sync::mpsc;
use std::time::Duration;

use harness::pgx::{Conn, ConnConfig};
use harness::server::{Server, Target};

/// connect connects to the server's `postgres` database without TLS.
fn connect(server: &Server) -> Conn {
    let url = format!("postgres://postgres:password@127.0.0.1:{}/postgres?sslmode=disable", server.port);
    Conn::connect(ConnConfig::parse(&url).unwrap()).unwrap()
}

/// exec runs a statement, failing the test when it fails.
fn exec(conn: &mut Conn, sql: &str) {
    conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
}

/// value returns the first column of a query's first row.
fn value(conn: &mut Conn, sql: &str) -> String {
    let result = conn.simple_query_rows(sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    let row = result.rows.first().unwrap_or_else(|| panic!("{sql}: no rows"));
    String::from_utf8(row[0].clone().unwrap_or_default()).unwrap()
}

/// vacuum runs VACUUM on the table on a connection of its own, failing when it has not returned within ten seconds.
fn vacuum(server: &Server) {
    let mut conn = connect(server);
    let (sender, receiver) = mpsc::channel();
    std::thread::spawn(move || {
        let _ = sender.send(conn.exec("VACUUM bv", &[]).map(drop).map_err(|err| err.to_string()));
    });
    let vacuumed = receiver.recv_timeout(Duration::from_secs(10)).expect("VACUUM waited for the table's writer");
    vacuumed.unwrap_or_else(|err| panic!("VACUUM bv: {err}"));
}

#[test]
fn test_vacuum_summarizes_brin_ranges_without_waiting_for_writers() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let (mut a, mut b) = (connect(&server), connect(&server));
    exec(&mut a, "CREATE TABLE bv (id INT PRIMARY KEY, a INT)");
    exec(&mut a, "INSERT INTO bv SELECT g, g FROM generate_series(1, 1000) g");
    exec(&mut a, "CREATE INDEX bv_a ON bv USING brin (a) WITH (pages_per_range = 1)");
    exec(&mut a, "SELECT brin_desummarize_range('bv_a', 0)");
    exec(&mut b, "BEGIN");
    exec(&mut b, "INSERT INTO bv VALUES (1001, 1001)");
    vacuum(&server);
    exec(&mut b, "COMMIT");
    let left = if target.is_postgres() { "0" } else { "1" };
    assert_eq!(value(&mut a, "SELECT brin_summarize_new_values('bv_a')"), left);
    exec(&mut a, "SELECT brin_desummarize_range('bv_a', 0)");
    exec(&mut b, "BEGIN");
    exec(&mut b, "INSERT INTO bv VALUES (1002, 1002)");
    vacuum(&server);
    exec(&mut b, "COMMIT");
    vacuum(&server);
    assert_eq!(value(&mut a, "SELECT brin_summarize_new_values('bv_a')"), "0");
    exec(&mut a, "SET enable_seqscan = off");
    assert_eq!(value(&mut a, "SELECT count(*) FROM bv WHERE a < 3 OR a > 1000"), "4");
}
