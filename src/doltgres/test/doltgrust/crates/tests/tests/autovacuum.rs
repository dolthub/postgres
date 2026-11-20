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

//! Tests that autovacuum analyzes tables whose changes pass its threshold, as Postgres' does. The server runs with an
//! `autovacuum_naptime` of one second; Postgres 18 with the same setting analyzed the first table about five seconds
//! after it was created and left the second alone.

use std::time::{Duration, Instant};

use harness::conn::{connect, value};
use harness::server::{Server, Target};

/// ROWS fills a table's insert with the 100 numbers from 0 to 99.
const ROWS: &str = "FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(x), \
                    (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(x)";

#[test]
fn test_autovacuum_analyzes_tables_past_their_threshold() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "system_variables:\n  autovacuum_naptime: 1\n").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE aa (id int PRIMARY KEY, a int, b text)".to_string(),
        format!("INSERT INTO aa SELECT a.x * 10 + b.x + 1, (a.x * 10 + b.x) % 7, 'x' || (a.x % 3) {ROWS}"),
        "CREATE TABLE ab (id int PRIMARY KEY) WITH (autovacuum_enabled = false)".to_string(),
        format!("INSERT INTO ab SELECT a.x * 10 + b.x {ROWS}"),
    ] {
        conn.exec(&sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let analyzed = "SELECT count(*) FROM pg_statistic WHERE starelid = 'aa'::regclass";
    let deadline = Instant::now() + Duration::from_secs(30);
    while value(&mut conn, analyzed) != "3" {
        assert!(Instant::now() < deadline, "autovacuum did not analyze the table");
        std::thread::sleep(Duration::from_millis(200));
    }
    assert_eq!(value(&mut conn, "SELECT reltuples FROM pg_class WHERE relname = 'aa'"), "100");
    let stats = "SELECT stadistinct || ' ' || stanumbers1::text FROM pg_statistic \
                 WHERE starelid = 'aa'::regclass AND staattnum = 3";
    assert_eq!(value(&mut conn, stats), "3 {0.4,0.3,0.3}");
    std::thread::sleep(Duration::from_secs(3));
    assert_eq!(value(&mut conn, "SELECT count(*) FROM pg_statistic WHERE starelid = 'ab'::regclass"), "0");
}

#[test]
fn test_autovacuum_analyzes_system_catalogs_past_their_threshold() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "system_variables:\n  autovacuum_naptime: 1\n").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = "DO $$ BEGIN FOR i IN 1..100 LOOP EXECUTE format('CREATE TABLE av%s (a INT)', i); END LOOP; END $$";
    conn.exec(create, &[]).unwrap();
    conn.exec("SELECT pg_stat_force_next_flush()", &[]).unwrap();
    let analyzed = "SELECT count(*) FROM pg_stat_sys_tables WHERE autoanalyze_count > 0 \
                    AND relname IN ('pg_class', 'pg_attribute', 'pg_type', 'pg_depend')";
    let deadline = Instant::now() + Duration::from_secs(30);
    while value(&mut conn, analyzed) != "4" {
        assert!(Instant::now() < deadline, "autovacuum did not analyze the system catalogs");
        std::thread::sleep(Duration::from_millis(200));
    }
    let rows = "SELECT reltuples = (SELECT count(*) FROM pg_class) FROM pg_class WHERE oid = 'pg_class'::regclass";
    assert_eq!(value(&mut conn, rows), "t");
}

#[test]
fn test_a_terminated_autovacuum_worker_logs_fatal() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let config = "system_variables:\n  autovacuum_naptime: 1\n  autovacuum_vacuum_cost_delay: 100\n  \
                  autovacuum_vacuum_cost_limit: 1\n";
    let server = Server::start(&target, config).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE slow (id int, b text)",
        "INSERT INTO slow SELECT g, repeat('b', 100) FROM generate_series(1, 10000) g",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let worker = "SELECT coalesce(max(pid), 0) FROM pg_stat_activity WHERE backend_type = 'autovacuum worker' \
                  AND query LIKE 'autovacuum: %slow%'";
    let deadline = Instant::now() + Duration::from_secs(30);
    let pid = loop {
        match value(&mut conn, worker) {
            pid if pid != "0" => break pid,
            _ => assert!(Instant::now() < deadline, "no autovacuum worker processed the table"),
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    assert_eq!(value(&mut conn, &format!("SELECT pg_terminate_backend({pid})")), "t");
    let line = "FATAL:  terminating autovacuum process due to administrator command";
    let deadline = Instant::now() + Duration::from_secs(10);
    let log = loop {
        let log = std::fs::read_to_string(server.directory().join("server.log")).unwrap_or_default();
        if log.contains(line) || Instant::now() >= deadline {
            break log;
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    assert!(log.contains(line), "{log}");
    assert!(!log.contains("terminating connection due to administrator command"), "{log}");
}
