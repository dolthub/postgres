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

//! Tests of statements that change the catalog in two sessions at once.

use std::time::Duration;

use harness::conn::connect;
use harness::pgx::Conn;
use harness::server::{Server, Target};

/// race runs the first session's statements in a transaction, then the second session's statement, which waits for
/// that transaction, then ends the transaction, and returns the second statement's outcome.
fn race(server: &Server, first: &[&str], second: &str, commit: bool) -> Result<String, harness::pgx::Error> {
    let mut a = connect(server, "postgres", "postgres");
    let mut b = connect(server, "postgres", "postgres");
    a.exec("BEGIN", &[]).unwrap();
    for sql in first {
        a.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let second = second.to_string();
    let waiter = std::thread::spawn(move || b.exec(&second, &[]));
    std::thread::sleep(Duration::from_millis(300));
    if waiter.is_finished() {
        panic!("the second statement did not wait for the first transaction: {:?}", waiter.join().unwrap());
    }
    a.exec(if commit { "COMMIT" } else { "ROLLBACK" }, &[]).unwrap();
    waiter.join().unwrap()
}

#[test]
fn test_creating_one_name_twice_at_once_fails_on_the_unique_catalog_index() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    for (first, second, index, detail) in [
        (
            &["CREATE TABLE c1 ()"][..],
            "CREATE TABLE c1 ()",
            "pg_type_typname_nsp_index",
            "Key (typname, typnamespace)=(c1, 2200) already exists.",
        ),
        (
            &["CREATE TABLE c2 ()", "ALTER TABLE c2 RENAME TO c3"][..],
            "CREATE TABLE c3 ()",
            "pg_type_typname_nsp_index",
            "Key (typname, typnamespace)=(c3, 2200) already exists.",
        ),
        (
            &["CREATE SCHEMA cs"][..],
            "CREATE SCHEMA cs",
            "pg_namespace_nspname_index",
            "Key (nspname)=(cs) already exists.",
        ),
    ] {
        let err = race(&server, first, second, true).expect_err(second);
        let fields = &err.pg_error().unwrap_or_else(|| panic!("{second}: {err}")).fields;
        assert_eq!(fields.code, "23505", "{second}: {err}");
        assert_eq!(fields.message, format!("duplicate key value violates unique constraint \"{index}\""));
        assert_eq!(fields.detail, detail);
        assert_eq!((fields.schema_name.as_str(), fields.constraint_name.as_str()), ("pg_catalog", index));
    }
    assert_eq!(race(&server, &["CREATE TABLE c4 ()"], "CREATE TABLE c4 ()", false).unwrap(), "CREATE TABLE");
}

#[test]
fn test_ddl_waits_for_the_locks_another_transactions_ddl_holds() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE w1 (a int)",
        "CREATE TABLE w2 (a int)",
        "CREATE TABLE w3 (a int)",
        "CREATE TABLE w4 (a int)",
        "CREATE SCHEMA ws",
        "CREATE FUNCTION wf() RETURNS int LANGUAGE sql AS 'SELECT 1'",
    ] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    assert_eq!(race(&server, &["ALTER TABLE w1 ADD COLUMN b int"], "DROP TABLE w1", true).unwrap(), "DROP TABLE");
    assert_eq!(
        race(&server, &["ALTER TABLE w4 ADD COLUMN b int"], "ALTER TABLE w4 ADD COLUMN b int", false).unwrap(),
        "ALTER TABLE"
    );
    for (first, second, code, message, detail) in [
        ("DROP TABLE w2", "ALTER TABLE w2 ADD COLUMN b int", "42P01", "relation \"w2\" does not exist", ""),
        ("ALTER TABLE w3 RENAME TO w5", "ALTER TABLE w3 ADD COLUMN c int", "42P01", "relation \"w3\" does not exist", ""),
        ("DROP FUNCTION wf()", "COMMENT ON FUNCTION wf() IS 'x'", "42883", "function wf() does not exist", ""),
        (
            "CREATE TABLE ws.t (a int)",
            "DROP SCHEMA ws",
            "2BP01",
            "cannot drop schema ws because other objects depend on it",
            "table ws.t depends on schema ws",
        ),
    ] {
        let err = race(&server, &[first], second, true).expect_err(second);
        let fields = &err.pg_error().unwrap_or_else(|| panic!("{second}: {err}")).fields;
        assert_eq!((fields.code.as_str(), fields.message.as_str()), (code, message), "{second}");
        assert_eq!(fields.detail, detail, "{second}");
    }
}

#[test]
fn test_select_and_ddl_wait_for_each_others_locks() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in ["CREATE TABLE sw1 (a int)", "CREATE TABLE sw2 (a int)", "CREATE TABLE sw3 (a int)"] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    assert_eq!(race(&server, &["SELECT * FROM sw1"], "DROP TABLE sw1", true).unwrap(), "DROP TABLE");
    assert_eq!(race(&server, &["ALTER TABLE sw2 ADD COLUMN b int"], "SELECT b FROM sw2", true).unwrap(), "SELECT 0");
    for (first, second, code, message) in [
        ("DROP TABLE sw3", "SELECT * FROM sw3", "42P01", "relation \"sw3\" does not exist"),
        ("ALTER TABLE sw2 RENAME COLUMN a TO c", "SELECT a FROM sw2", "42703", "column \"a\" does not exist"),
    ] {
        let err = race(&server, &[first], second, true).expect_err(second);
        let fields = &err.pg_error().unwrap_or_else(|| panic!("{second}: {err}")).fields;
        assert_eq!((fields.code.as_str(), fields.message.as_str()), (code, message), "{second}");
    }
}

#[test]
fn test_lock_table_holds_its_locks_until_the_transaction_ends() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE lt (a int)",
        "CREATE TABLE lt2 (a int)",
        "CREATE VIEW ltv AS SELECT a FROM lt2",
        "CREATE SEQUENCE lts",
    ] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let err = setup.exec("LOCK TABLE lt", &[]).unwrap_err();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!(
        (fields.code.as_str(), fields.message.as_str()),
        ("25P01", "LOCK TABLE can only be used in transaction blocks")
    );
    assert_eq!(race(&server, &["LOCK TABLE lt"], "SELECT * FROM lt", true).unwrap(), "SELECT 0");
    assert_eq!(race(&server, &["LOCK TABLE ltv"], "SELECT * FROM lt2", true).unwrap(), "SELECT 0");
    let mut a = connect(&server, "postgres", "postgres");
    let mut b = connect(&server, "postgres", "postgres");
    a.exec("BEGIN; LOCK TABLE lt IN EXCLUSIVE MODE", &[]).unwrap();
    for (sql, message) in [
        ("BEGIN; LOCK TABLE lt IN SHARE MODE NOWAIT", "could not obtain lock on relation \"lt\""),
        ("BEGIN; LOCK TABLE public.lt NOWAIT", "could not obtain lock on relation \"public.lt\""),
    ] {
        let err = b.exec(sql, &[]).unwrap_err();
        let fields = &err.pg_error().unwrap().fields;
        assert_eq!((fields.code.as_str(), fields.message.as_str()), ("55P03", message), "{sql}");
        b.exec("ROLLBACK", &[]).unwrap();
    }
    assert_eq!(b.exec("BEGIN; LOCK TABLE lt IN ACCESS SHARE MODE NOWAIT", &[]).unwrap(), "LOCK TABLE");
    b.exec("ROLLBACK", &[]).unwrap();
    a.exec("ROLLBACK", &[]).unwrap();
    let err = b.exec("BEGIN; LOCK TABLE lts", &[]).unwrap_err();
    let fields = &err.pg_error().unwrap().fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("42809", "cannot lock relation \"lts\""));
    assert_eq!(fields.detail, "This operation is not supported for sequences.");
}

/// value runs a query and returns the text of its first row's first column.
fn value(conn: &mut Conn, sql: &str) -> String {
    let result = conn.query_with_result_format(sql, 0).unwrap_or_else(|err| panic!("{sql}: {err}"));
    String::from_utf8(result.rows[0][0].clone().unwrap()).unwrap()
}

#[test]
fn test_a_sessions_exit_waits_for_another_sessions_drop_of_its_temporary_table() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let count = "SELECT count(*) FROM pg_class WHERE relname IN ('xt1', 'xt2')";
    for commit in [true, false] {
        let mut a = connect(&server, "postgres", "postgres");
        let mut b = connect(&server, "postgres", "postgres");
        let mut c = connect(&server, "postgres", "postgres");
        a.exec("CREATE TEMP TABLE xt1 (a INT)", &[]).unwrap();
        a.exec("CREATE TEMP TABLE xt2 (a INT)", &[]).unwrap();
        let schema = value(&mut a, "SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema()");
        b.exec("BEGIN", &[]).unwrap();
        assert_eq!(b.exec(&format!("DROP TABLE {schema}.xt1"), &[]).unwrap(), "DROP TABLE");
        a.close();
        std::thread::sleep(Duration::from_millis(300));
        assert_eq!(value(&mut c, count), "2", "the exit dropped the tables before the other transaction ended");
        b.exec(if commit { "COMMIT" } else { "ROLLBACK" }, &[]).unwrap();
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        while value(&mut c, count) != "0" {
            assert!(std::time::Instant::now() < deadline, "the exit did not drop the tables (commit: {commit})");
            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

#[test]
fn test_drop_role_and_a_new_owner_or_grantee_wait_for_each_other() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE ROLE dr1",
        "CREATE ROLE dr2",
        "CREATE ROLE dr3",
        "CREATE TABLE drt1 (a INT)",
        "CREATE TABLE drt2 (a INT)",
    ] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let err = race(&server, &["ALTER TABLE drt1 OWNER TO dr1"], "DROP ROLE dr1", true).expect_err("DROP ROLE dr1");
    let fields = &err.pg_error().unwrap_or_else(|| panic!("DROP ROLE dr1: {err}")).fields;
    assert_eq!(fields.code, "2BP01", "{err}");
    assert_eq!(fields.message, "role \"dr1\" cannot be dropped because some objects depend on it");
    assert_eq!(fields.detail, "owner of table drt1");
    for (role, second) in [("dr2", "ALTER TABLE drt2 OWNER TO dr2"), ("dr3", "GRANT SELECT ON drt2 TO dr3")] {
        let oid = value(&mut setup, &format!("SELECT oid FROM pg_authid WHERE rolname = '{role}'"));
        let err = race(&server, &[&format!("DROP ROLE {role}")], second, true).expect_err(second);
        let fields = &err.pg_error().unwrap_or_else(|| panic!("{second}: {err}")).fields;
        assert_eq!(fields.code, "42704", "{second}: {err}");
        assert_eq!(fields.message, format!("role {oid} was concurrently dropped"), "{second}");
    }
    assert_eq!(value(&mut setup, "SELECT relowner::regrole::text FROM pg_class WHERE relname = 'drt2'"), "postgres");
}

#[test]
fn test_a_role_commands_commit_waits_out_a_database_lock_past_its_lock_timeout() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in ["CREATE DATABASE regress_held", "CREATE DATABASE regress_gone"] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let mut holder = connect(&server, "postgres", "postgres");
    let mut role = connect(&server, "postgres", "postgres");
    role.exec("SET lock_timeout = 100", &[]).unwrap();
    holder.exec("BEGIN", &[]).unwrap();
    holder.exec("ALTER DATABASE regress_held RENAME TO regress_kept", &[]).unwrap();
    let waiter = std::thread::spawn(move || role.exec("CREATE ROLE regress_held_role", &[]));
    std::thread::sleep(Duration::from_millis(300));
    assert!(!waiter.is_finished(), "the commit did not wait for the database's lock");
    holder.exec("COMMIT", &[]).unwrap();
    assert_eq!(waiter.join().unwrap().unwrap(), "CREATE ROLE");
    let mut kept = connect(&server, "regress_kept", "postgres");
    assert_eq!(value(&mut kept, "SELECT count(*) FROM pg_roles WHERE rolname = 'regress_held_role'"), "1");
    let mut comment = connect(&server, "postgres", "postgres");
    comment.exec("SET lock_timeout = 100", &[]).unwrap();
    comment.exec("BEGIN", &[]).unwrap();
    comment.exec("COMMENT ON DATABASE regress_gone IS 'held'", &[]).unwrap();
    holder.exec("BEGIN", &[]).unwrap();
    holder.exec("CREATE ROLE regress_gone_role", &[]).unwrap();
    let mut dropper = connect(&server, "postgres", "postgres");
    let dropped = std::thread::spawn(move || dropper.exec("DROP DATABASE regress_gone", &[]));
    std::thread::sleep(Duration::from_millis(300));
    let commented = std::thread::spawn(move || comment.exec("COMMIT", &[]));
    std::thread::sleep(Duration::from_millis(300));
    assert!(!dropped.is_finished() && !commented.is_finished(), "the drop or the commit did not wait");
    let held = std::thread::spawn(move || holder.exec("COMMIT", &[]));
    assert_eq!(dropped.join().unwrap().unwrap(), "DROP DATABASE");
    assert_eq!(commented.join().unwrap().unwrap(), "COMMIT");
    assert_eq!(held.join().unwrap().unwrap(), "COMMIT");
    assert_eq!(value(&mut setup, "SELECT count(*) FROM pg_roles WHERE rolname = 'regress_gone_role'"), "1");
}

#[test]
fn test_a_database_move_waits_for_writes_into_its_tree_from_another_database() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut setup = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE DATABASE regress_moving",
        "SET allow_in_place_tablespaces = on",
        "CREATE TABLESPACE regress_ts LOCATION ''",
    ] {
        setup.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let comment = "COMMENT ON DATABASE regress_moving IS 'moved'";
    let moved = race(&server, &[comment], "ALTER DATABASE regress_moving SET TABLESPACE regress_ts", true);
    assert_eq!(moved.unwrap(), "ALTER DATABASE");
    if !target.is_postgres() {
        let role = "CREATE ROLE regress_moving_role";
        let moved = race(&server, &[role], "ALTER DATABASE regress_moving SET TABLESPACE pg_default", true);
        assert_eq!(moved.unwrap(), "ALTER DATABASE");
    }
    let mut moving = connect(&server, "regress_moving", "postgres");
    let description = "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = current_database()";
    assert_eq!(value(&mut moving, description), "moved");
    if !target.is_postgres() {
        assert_eq!(value(&mut moving, "SELECT count(*) FROM pg_roles WHERE rolname = 'regress_moving_role'"), "1");
    }
}
