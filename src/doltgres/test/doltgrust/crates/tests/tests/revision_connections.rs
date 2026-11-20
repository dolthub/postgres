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

//! Tests of connections to a revision of a database, named `<database>/<revision>` or `<database>@<revision>` at
//! login: a branch opens a session on it, and `current_branch()` names the session's database and branch.

use harness::conn::{connect, error, expected, refused, rows, run, value};
use harness::server::{Server, Target};

#[test]
fn test_a_connection_to_a_branch_opens_a_session_on_it() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY)", "SELECT dolt_commit('-Am', 'one')", "SELECT dolt_branch('b1')"]);
    let names = "SELECT current_branch(), current_database(), active_branch()";
    assert_eq!(rows(&mut main, names), expected(&[&["postgres/main", "postgres", "main"]]));
    let mut slash = connect(&server, "postgres/b1", "postgres");
    assert_eq!(rows(&mut slash, names), expected(&[&["postgres/b1", "postgres", "b1"]]));
    run(&mut slash, &["INSERT INTO t VALUES (1)"]);
    let mut at = connect(&server, "postgres@b1", "postgres");
    assert_eq!(rows(&mut at, names), expected(&[&["postgres/b1", "postgres", "b1"]]));
    assert_eq!(value(&mut at, "SELECT count(*) FROM t"), "1");
    assert_eq!(value(&mut main, "SELECT count(*) FROM t"), "0");
    run(&mut main, &["SELECT dolt_checkout('b1')"]);
    assert_eq!(rows(&mut main, names), expected(&[&["postgres/b1", "postgres", "b1"]]));
    assert_eq!(value(&mut main, "SELECT count(*) FROM t"), "1");
}

#[test]
fn test_connections_to_a_missing_revision_are_refused() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY)", "SELECT dolt_commit('-Am', 'one')"]);
    let missing = |name: &str| ("3D000".to_string(), format!("database \"{name}\" does not exist"));
    assert_eq!(refused(&server, "postgres/nosuch", "postgres"), missing("postgres/nosuch"));
    assert_eq!(refused(&server, "postgres/main~5", "postgres"), missing("postgres/main~5"));
    assert_eq!(refused(&server, "nosuch/main", "postgres"), missing("nosuch/main"));
    assert_eq!(refused(&server, "postgres/", "postgres"), missing("postgres/"));
}

#[test]
fn test_database_names_holding_revision_marks_are_refused() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    let detail = "The characters \"/\" and \"@\" mark a revision of a database.".to_string();
    let unacceptable = |name: &str| ("42602".to_string(), format!("unacceptable database name \"{name}\""), detail.clone());
    assert_eq!(error(&mut main, "CREATE DATABASE \"a/b\""), unacceptable("a/b"));
    assert_eq!(error(&mut main, "CREATE DATABASE \"a@b\""), unacceptable("a@b"));
    run(&mut main, &["CREATE DATABASE plain"]);
    assert_eq!(error(&mut main, "ALTER DATABASE plain RENAME TO \"p/q\""), unacceptable("p/q"));
    assert_eq!(value(&mut main, "SELECT count(*) FROM pg_database WHERE datname = 'plain'"), "1");
}

#[test]
fn test_a_connection_to_a_commit_reads_the_commit() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(
        &mut main,
        &[
            "CREATE TABLE t (pk INT PRIMARY KEY, v TEXT)",
            "CREATE INDEX t_v ON t (v)",
            "INSERT INTO t VALUES (1, 'b'), (2, 'a')",
            "SELECT dolt_commit('-Am', 'one')",
            "INSERT INTO t VALUES (3, 'c')",
            "ALTER TABLE t ADD COLUMN w INT",
            "SELECT dolt_commit('-Am', 'two')",
            "INSERT INTO t VALUES (4, 'd', 4)",
        ],
    );
    let hash = value(&mut main, "SELECT dolt_hashof('HEAD~1')");
    let one = expected(&[&["2", "a"], &["1", "b"]]);
    for name in [format!("postgres/{hash}"), "postgres/main~1".to_string(), format!("postgres@{}", &hash[..10])] {
        let mut session = connect(&server, &name, "postgres");
        assert_eq!(rows(&mut session, "SELECT pk, v FROM t ORDER BY v"), one, "{name}");
        assert_eq!(value(&mut session, "SELECT count(*) FROM t WHERE v = 'a'"), "1", "{name}");
    }
    let mut head = connect(&server, &format!("postgres/{}", value(&mut main, "SELECT dolt_hashof('HEAD')")), "postgres");
    assert_eq!(value(&mut head, "SELECT count(*) FROM t"), "3");
    assert_eq!(value(&mut main, "SELECT count(*) FROM t"), "4");
}

#[test]
fn test_a_connection_to_a_commit_refuses_writes_and_read_write_mode() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY)", "CREATE SEQUENCE s", "SELECT dolt_commit('-Am', 'one')"]);
    let mut session = connect(&server, &format!("postgres/{}", value(&mut main, "SELECT dolt_hashof('HEAD')")), "postgres");
    let read_only = |command: &str| {
        ("25006".to_string(), format!("cannot execute {command} in a read-only transaction"), String::new())
    };
    let read_write = (
        "0A000".to_string(),
        "cannot set transaction read-write mode in a session on a revision of a database".to_string(),
        String::new(),
    );
    assert_eq!(error(&mut session, "INSERT INTO t VALUES (1)"), read_only("INSERT"));
    assert_eq!(error(&mut session, "CREATE TABLE u (pk INT)"), read_only("CREATE TABLE"));
    assert_eq!(error(&mut session, "SELECT nextval('s')"), read_only("nextval()"));
    assert_eq!(error(&mut session, "CREATE TEMP TABLE tmp (pk INT)"), read_only("CREATE TABLE"));
    assert_eq!(error(&mut session, "SET transaction_read_only = off"), read_write);
    assert_eq!(error(&mut session, "BEGIN READ WRITE"), read_write);
    assert_eq!(error(&mut session, "START TRANSACTION READ WRITE"), read_write);
    run(&mut session, &["BEGIN"]);
    assert_eq!(error(&mut session, "SET TRANSACTION READ WRITE"), read_write);
    run(&mut session, &["ROLLBACK", "SET default_transaction_read_only = off"]);
    assert_eq!(error(&mut session, "INSERT INTO t VALUES (1)"), read_only("INSERT"));
    run(&mut session, &["SET SESSION CHARACTERISTICS AS TRANSACTION READ WRITE"]);
    assert_eq!(error(&mut session, "INSERT INTO t VALUES (1)"), read_only("INSERT"));
    assert_eq!(value(&mut session, "SHOW transaction_read_only"), "on");
    assert_eq!(value(&mut main, "SELECT count(*) FROM t"), "0");
}

#[test]
fn test_a_connection_to_a_commit_names_the_revision_and_reads_version_control_from_it() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY)", "SELECT dolt_commit('-Am', 'one')"]);
    run(&mut main, &["INSERT INTO t VALUES (1)", "SELECT dolt_commit('-am', 'two')", "INSERT INTO t VALUES (2)"]);
    let one = value(&mut main, "SELECT dolt_hashof('HEAD~1')");
    let mut session = connect(&server, "postgres/main~1", "postgres");
    let names = "SELECT current_branch(), current_database(), active_branch() IS NULL";
    assert_eq!(rows(&mut session, names), expected(&[&["postgres/main~1", "postgres", "t"]]));
    for revision in ["HEAD", "main~1"] {
        assert_eq!(value(&mut session, &format!("SELECT dolt_hashof('{revision}')")), one, "{revision}");
    }
    for revision in ["WORKING", "STAGED"] {
        let refused = ("22023".to_string(), "invalid ref spec".to_string(), String::new());
        assert_eq!(error(&mut session, &format!("SELECT dolt_hashof('{revision}')")), refused, "{revision}");
    }
    assert_eq!(value(&mut session, "SELECT count(*) FROM dolt_status"), "0");
    assert_eq!(value(&mut session, "SELECT message FROM dolt_log LIMIT 1"), "one");
    assert_eq!(value(&mut session, "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 't')"), "0");
    assert_eq!(value(&mut session, "SELECT count(*) FROM t AS OF 'main'"), "1");
    assert_eq!(value(&mut main, "SELECT count(*) FROM dolt_status"), "1");
}

#[test]
fn test_a_connection_to_a_commit_makes_branches_and_checks_them_out() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY)", "SELECT dolt_commit('-Am', 'one')"]);
    run(&mut main, &["INSERT INTO t VALUES (1)", "SELECT dolt_commit('-am', 'two')"]);
    let one = value(&mut main, "SELECT dolt_hashof('HEAD~1')");
    let mut session = connect(&server, &format!("postgres/{one}"), "postgres");
    run(&mut session, &["SELECT dolt_branch('b1')"]);
    assert_eq!(value(&mut main, "SELECT dolt_hashof('b1')"), one);
    run(&mut session, &["SELECT dolt_checkout('-b', 'b2')"]);
    let names = "SELECT current_branch(), active_branch()";
    assert_eq!(rows(&mut session, names), expected(&[&["postgres/b2", "b2"]]));
    run(&mut session, &["INSERT INTO t VALUES (5)"]);
    assert_eq!(value(&mut session, "SELECT count(*) FROM t"), "1");
    let mut other = connect(&server, &format!("postgres/{one}"), "postgres");
    run(&mut other, &["SELECT dolt_checkout('main')"]);
    assert_eq!(rows(&mut other, names), expected(&[&["postgres/main", "main"]]));
    assert_eq!(value(&mut other, "SELECT count(*) FROM t"), "1");
    run(&mut other, &["INSERT INTO t VALUES (2)"]);
    run(&mut main, &["BEGIN READ ONLY"]);
    let read_only = |command: &str| {
        ("25006".to_string(), format!("cannot execute {command} in a read-only transaction"), String::new())
    };
    assert_eq!(error(&mut main, "SELECT dolt_branch('b3')"), read_only("dolt_branch()"));
    run(&mut main, &["ROLLBACK", "BEGIN READ ONLY"]);
    assert_eq!(error(&mut main, "SELECT dolt_checkout('-b', 'b3')"), read_only("dolt_checkout()"));
    run(&mut main, &["ROLLBACK"]);
    assert_eq!(value(&mut main, "SELECT count(*) FROM dolt_branches WHERE name = 'b3'"), "0");
}

#[test]
fn test_a_connection_to_a_commit_scans_its_collated_indexes() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(
        &mut main,
        &[
            "CREATE TABLE c (pk INT PRIMARY KEY, a TEXT COLLATE \"C\", b TEXT COLLATE \"en-x-icu\")",
            "CREATE INDEX c_a ON c (a)",
            "CREATE INDEX c_b ON c (b)",
            "INSERT INTO c VALUES (1, 'a', 'a'), (2, 'B', 'B'), (3, 'c', 'c')",
            "SELECT dolt_commit('-Am', 'one')",
        ],
    );
    let hash = value(&mut main, "SELECT dolt_hashof('HEAD')");
    let mut session = connect(&server, &format!("postgres/{hash}"), "postgres");
    run(&mut main, &["SET enable_seqscan = off"]);
    run(&mut session, &["SET enable_seqscan = off"]);
    for (column, order) in [("a", ["B", "a", "c"]), ("b", ["a", "B", "c"])] {
        let explain = format!("EXPLAIN (COSTS OFF) SELECT {column} FROM c WHERE {column} > '' ORDER BY {column}");
        let plan = rows(&mut session, &explain);
        assert_eq!(plan, rows(&mut main, &explain), "{column}");
        assert!(plan[0][0].contains(&format!("using c_{column}")), "{plan:?}");
        let query = format!("SELECT {column} FROM c WHERE {column} > '' ORDER BY {column}");
        assert_eq!(rows(&mut session, &query), expected(&[&[order[0]], &[order[1]], &[order[2]]]), "{column}");
    }
}

#[test]

fn test_a_connection_to_a_commit_lists_the_columns_of_the_commit() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    run(&mut main, &["CREATE TABLE t (pk INT PRIMARY KEY, v TEXT)", "SELECT dolt_commit('-Am', 'one')"]);
    run(&mut main, &["ALTER TABLE t ADD COLUMN w INT", "SELECT dolt_commit('-Am', 'two')"]);
    let mut session = connect(&server, "postgres/main~1", "postgres");
    let columns = "SELECT attname FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum";
    assert_eq!(rows(&mut session, columns), expected(&[&["pk"], &["v"]]));
}
