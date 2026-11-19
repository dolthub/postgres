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

//! Tests of databases whose tablespace is not `pg_default`, which a script test's database cannot be.

use harness::conn::{connect, error, expected, rows, run, value};
use harness::server::{Server, Target};

#[test]
fn test_relations_move_between_pg_default_and_their_databases_tablespace() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    run(
        &mut setup,
        &[
            "SET allow_in_place_tablespaces = on",
            "CREATE TABLESPACE regress_ts LOCATION ''",
            "CREATE DATABASE regress_moved TABLESPACE regress_ts",
        ],
    );
    let mut moved = connect(&server, "regress_moved", "postgres");
    run(&mut moved, &["CREATE TABLE t (a INT)", "CREATE INDEX ti ON t (a)"]);
    let placed = "SELECT relname, reltablespace FROM pg_class WHERE relname IN ('t', 'ti') ORDER BY relname";
    let spaces = |conn: &mut harness::pgx::Conn| rows(conn, placed);
    assert_eq!(spaces(&mut moved), expected(&[&["t", "0"], &["ti", "0"]]));
    run(&mut moved, &["ALTER TABLE t SET TABLESPACE pg_default", "ALTER INDEX ti SET TABLESPACE pg_default"]);
    assert_eq!(spaces(&mut moved), expected(&[&["t", "1663"], &["ti", "1663"]]));
    run(&mut moved, &["ALTER TABLE t SET TABLESPACE regress_ts", "REINDEX (TABLESPACE regress_ts) INDEX ti"]);
    assert_eq!(spaces(&mut moved), expected(&[&["t", "0"], &["ti", "0"]]));
    run(&mut moved, &["REINDEX (TABLESPACE pg_default) TABLE t"]);
    assert_eq!(spaces(&mut moved), expected(&[&["t", "0"], &["ti", "1663"]]));
    let location = "SELECT pg_tablespace_location(0) = pg_tablespace_location(oid) AND pg_tablespace_location(0) <> '' \
                    FROM pg_tablespace WHERE spcname = 'regress_ts'";
    assert_eq!(value(&mut moved, location), "t");
    assert_eq!(value(&mut setup, "SELECT pg_tablespace_location(0)"), "");
}

#[test]
fn test_sizes_need_no_privilege_on_the_databases_own_tablespace() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    run(
        &mut setup,
        &[
            "SET allow_in_place_tablespaces = on",
            "CREATE TABLESPACE regress_ts LOCATION ''",
            "CREATE DATABASE regress_sized TABLESPACE regress_ts",
            "CREATE ROLE regress_plain",
        ],
    );
    let mut sized = connect(&server, "regress_sized", "postgres");
    run(&mut sized, &["SET ROLE regress_plain"]);
    assert_eq!(value(&mut sized, "SELECT pg_tablespace_size('regress_ts') > 0"), "t");
    let denied = error(&mut sized, "SELECT pg_tablespace_size('pg_default')");
    assert_eq!(denied, ("42501".into(), "permission denied for tablespace pg_default".into(), String::new()));
}

#[test]
fn test_sessions_place_temporary_tables_in_their_own_temporary_tablespaces() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    run(
        &mut setup,
        &[
            "SET allow_in_place_tablespaces = on",
            "CREATE TABLESPACE regress_ts1 LOCATION ''",
            "CREATE TABLESPACE regress_ts2 LOCATION ''",
        ],
    );
    let (mut first, mut second) = (connect(&server, "postgres", "postgres"), connect(&server, "postgres", "postgres"));
    run(&mut first, &["SET temp_tablespaces = regress_ts1"]);
    run(&mut second, &["SET temp_tablespaces = regress_ts2"]);
    for _ in 0..3 {
        run(&mut first, &["CREATE TEMP TABLE IF NOT EXISTS t (a INT)", "INSERT INTO t VALUES (1)"]);
        run(&mut second, &["CREATE TEMP TABLE IF NOT EXISTS t (a INT)", "INSERT INTO t VALUES (2)"]);
    }
    let placed = "SELECT t.spcname FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace \
                  WHERE c.oid = 'pg_temp.t'::regclass";
    assert_eq!(value(&mut first, placed), "regress_ts1");
    assert_eq!(value(&mut second, placed), "regress_ts2");
}
