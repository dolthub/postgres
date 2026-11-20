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

//! Helpers of tests that drive a server through connections of their own: connecting, running statements that
//! succeed, and reading what fails or returns.

use crate::pgx::{Conn, ConnConfig};
use crate::server::Server;

/// connect connects to a database of a server as a user whose password is `password`.
pub fn connect(server: &Server, database: &str, user: &str) -> Conn {
    let url = format!("postgres://{user}:password@127.0.0.1:{}/{database}?sslmode=disable", server.port);
    Conn::connect(ConnConfig::parse(&url).unwrap()).unwrap_or_else(|err| panic!("{user}@{database}: {err}"))
}

/// refused returns the SQLSTATE and message of a login that fails.
pub fn refused(server: &Server, database: &str, user: &str) -> (String, String) {
    let url = format!("postgres://{user}:password@127.0.0.1:{}/{database}?sslmode=disable", server.port);
    let Err(err) = Conn::connect(ConnConfig::parse(&url).unwrap()) else { panic!("{user} connected to {database}") };
    let fields = &err.pg_error().unwrap_or_else(|| panic!("{err}")).fields;
    (fields.code.clone(), fields.message.clone())
}

/// run runs statements that succeed.
pub fn run(conn: &mut Conn, statements: &[&str]) {
    for sql in statements {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
}

/// error returns the SQLSTATE, message, and detail of a statement that fails.
pub fn error(conn: &mut Conn, sql: &str) -> (String, String, String) {
    let err = conn.exec(sql, &[]).expect_err(sql);
    let fields = &err.pg_error().unwrap_or_else(|| panic!("{sql}: {err}")).fields;
    (fields.code.clone(), fields.message.clone(), fields.detail.clone())
}

/// rows returns a query's rows as text.
pub fn rows(conn: &mut Conn, sql: &str) -> Vec<Vec<String>> {
    let result = conn.simple_query_rows(sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    let text = |cell: &Option<Vec<u8>>| String::from_utf8(cell.clone().unwrap_or_default()).unwrap();
    result.rows.iter().map(|row| row.iter().map(text).collect()).collect()
}

/// expected returns rows of text cells.
pub fn expected(rows: &[&[&str]]) -> Vec<Vec<String>> {
    rows.iter().map(|row| row.iter().map(|cell| cell.to_string()).collect()).collect()
}

/// value returns the first column of a query's first row.
pub fn value(conn: &mut Conn, sql: &str) -> String {
    let result = conn.simple_query_rows(sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    let row = result.rows.first().unwrap_or_else(|| panic!("{sql}: no rows"));
    String::from_utf8(row[0].clone().unwrap_or_default()).unwrap()
}
