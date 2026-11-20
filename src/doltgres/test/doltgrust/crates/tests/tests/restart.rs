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

//! Tests that what statements commit is still there after the server restarts.

use harness::conn::{connect, error, rows, run, value};
use harness::pgx::Conn;
use harness::server::{Server, Target};

/// error_code runs a statement that fails and returns its SQLSTATE.
fn error_code(conn: &mut Conn, sql: &str) -> String {
    let err = conn.exec(sql, &[]).expect_err(sql);
    err.pg_error().unwrap_or_else(|| panic!("{sql}: {err}")).fields.code.clone()
}

#[test]
fn test_ddl_survives_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE SCHEMA s",
        "CREATE TABLE s.t (a int PRIMARY KEY, b text DEFAULT 'x')",
        "CREATE INDEX t_b ON s.t (b)",
        "CREATE VIEW s.v AS SELECT 1 AS one",
        "CREATE DATABASE d2",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.exec("BEGIN", &[]).unwrap();
    conn.exec("CREATE TABLE s.rolled_back (a int)", &[]).unwrap();
    conn.exec("ROLLBACK", &[]).unwrap();
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(error_code(&mut conn, "CREATE SCHEMA s"), "42P06");
    assert_eq!(error_code(&mut conn, "CREATE TABLE s.t (a int)"), "42P07");
    assert_eq!(error_code(&mut conn, "CREATE DATABASE d2"), "42P04");
    assert_eq!(error_code(&mut conn, "DROP TABLE s.rolled_back"), "42P01");
    for (sql, tag) in [
        ("DROP INDEX s.t_b", "DROP INDEX"),
        ("DROP VIEW s.v", "DROP VIEW"),
        ("DROP TABLE s.t", "DROP TABLE"),
        ("CREATE TABLE s.t (a int)", "CREATE TABLE"),
    ] {
        assert_eq!(conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}")), tag);
    }
    conn.close();
    let mut conn = connect(&server, "d2", "postgres");
    assert_eq!(conn.exec("CREATE TABLE u (a int)", &[]).unwrap(), "CREATE TABLE");
    conn.close();
}

#[test]
fn test_role_oids_survive_a_restart_and_come_with_a_template() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "behavior:\n  server_wide_roles: false\n").unwrap();
    let mut conn = connect(&server, "template1", "postgres");
    for sql in ["CREATE ROLE ro_a", "CREATE ROLE ro_b", "GRANT ro_a TO ro_b"] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let role = "SELECT oid FROM pg_authid WHERE rolname = 'ro_a'";
    let member = "SELECT oid FROM pg_auth_members WHERE member = 'ro_b'::regrole";
    let (a, membership) = (value(&mut conn, role), value(&mut conn, member));
    conn.exec("CREATE DATABASE ro_copy", &[]).unwrap();
    conn.close();
    let mut conn = connect(&server, "ro_copy", "postgres");
    assert_eq!(value(&mut conn, role), a);
    assert_eq!(value(&mut conn, member), membership);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "template1", "postgres");
    assert_eq!(value(&mut conn, role), a);
    assert_eq!(value(&mut conn, member), membership);
    conn.close();
    let mut conn = connect(&server, "ro_copy", "postgres");
    assert_eq!(value(&mut conn, role), a);
    assert_eq!(value(&mut conn, member), membership);
    conn.close();
}

#[test]
fn test_gist_indexes_keep_their_unions_after_a_restart_and_a_persistence_change() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE g (id int PRIMARY KEY, p point)",
        "INSERT INTO g SELECT i, point(i % 50, i / 50) FROM generate_series(1, 3000) i",
        "CREATE INDEX g_p ON g USING gist (p)",
        "CREATE TABLE u (id int PRIMARY KEY, p point)",
        "INSERT INTO u SELECT i, point(i % 50, i / 50) FROM generate_series(1, 3000) i",
        "CREATE INDEX u_p ON u USING gist (p)",
        "CHECKPOINT",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let inside = "SELECT count(*) FROM g WHERE p <@ box '(0,0),(10,10)'";
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    conn.exec(&format!("EXPLAIN {inside}"), &[]).unwrap();
    for sql in [
        "INSERT INTO g SELECT i, point(i % 50, i / 50) FROM generate_series(3001, 6000) i",
        "VACUUM g",
        "ALTER TABLE u SET UNLOGGED",
        "INSERT INTO u SELECT i, point(i % 50, i / 50) FROM generate_series(3001, 6000) i",
        "VACUUM u",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    assert_eq!(value(&mut conn, inside), "120");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM u WHERE p <@ box '(0,0),(10,10)'"), "120");
    conn.close();
}

#[test]
fn test_comparator_indexes_keep_their_order_after_a_restart_and_a_recovery() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE FUNCTION rev_cmp(int4, int4) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END $$",
        "CREATE OPERATOR ~<~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = int4gt)",
        "CREATE OPERATOR ~<=~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = int4ge)",
        "CREATE OPERATOR ~>=~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = int4le)",
        "CREATE OPERATOR ~>~ (LEFTARG = int4, RIGHTARG = int4, FUNCTION = int4lt)",
        "CREATE OPERATOR CLASS rev_ops FOR TYPE int4 USING btree AS OPERATOR 1 ~<~, OPERATOR 2 ~<=~, OPERATOR 3 =, OPERATOR 4 ~>=~, OPERATOR 5 ~>~, FUNCTION 1 rev_cmp(int4, int4)",
        "CREATE TABLE r (id int PRIMARY KEY, a int)",
        "INSERT INTO r SELECT i, i FROM generate_series(1, 2000) i",
        "CREATE INDEX r_a ON r (a rev_ops)",
        "CHECKPOINT",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    let top = "SELECT string_agg(a::text, ',') FROM (SELECT a FROM r WHERE a ~>=~ 2500 ORDER BY a USING ~<~ LIMIT 3) s";
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert!(value(&mut conn, &format!("EXPLAIN (COSTS OFF) {top}")).contains("Aggregate"));
    assert_eq!(value(&mut conn, top), "2000,1999,1998");
    for sql in ["INSERT INTO r SELECT i, i FROM generate_series(2001, 2400) i", "DELETE FROM r WHERE id = 2400"] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert_eq!(value(&mut conn, top), "2399,2398,2397");
    conn.exec("VACUUM r", &[]).unwrap();
    assert_eq!(value(&mut conn, top), "2399,2398,2397");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM r WHERE a ~<~ 100"), "2299");
    conn.close();
}

/// test_comparator_indexes_whose_folds_fail_stay_broken_until_a_reindex is Doltgrust's own behavior: a comparison
/// function replaced by one that fails leaves a fold of its index's changes broken, where Postgres fails the reads.
#[test]
fn test_comparator_indexes_whose_folds_fail_stay_broken_until_a_reindex() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let compare = "CREATE OR REPLACE FUNCTION my_cmp(int4, int4) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$";
    for sql in [
        compare,
        "CREATE OPERATOR CLASS my_ops FOR TYPE int4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 my_cmp(int4, int4)",
        "CREATE TABLE k (id int PRIMARY KEY, a int, b int)",
        "CREATE UNIQUE INDEX k_a ON k (a my_ops)",
        "CREATE INDEX k_b ON k (b my_ops)",
        "INSERT INTO k SELECT i, i, i FROM generate_series(1, 100) i",
        "CREATE OR REPLACE FUNCTION my_cmp(int4, int4) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT $1 / ($2 - $2) $$",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    assert_eq!(error_code(&mut conn, "VACUUM k"), "22012");
    conn.exec("CHECKPOINT", &[]).unwrap();
    assert_eq!(conn.exec("VACUUM k", &[]).unwrap(), "VACUUM");
    let log = std::fs::read_to_string(server.directory().join("server.log")).unwrap();
    assert!(log.contains("index \"k_a\" is invalid until it is reindexed"), "{log}");
    assert!(log.contains("index \"k_b\" is invalid until it is reindexed"), "{log}");
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert!(value(&mut conn, "EXPLAIN (COSTS OFF) SELECT a FROM k WHERE a = 5").starts_with("Seq Scan on k"));
    assert_eq!(value(&mut conn, "SELECT count(*) FROM k WHERE a = 5 AND b = 5"), "1");
    assert_eq!(error_code(&mut conn, "INSERT INTO k VALUES (101, 101, 101)"), "55000");
    assert_eq!(error_code(&mut conn, "UPDATE k SET a = 200 WHERE id = 1"), "55000");
    for (sql, tag) in [("UPDATE k SET b = 200 WHERE id = 1", "UPDATE 1"), ("DELETE FROM k WHERE id = 2", "DELETE 1"), ("CHECKPOINT", "CHECKPOINT")] {
        assert_eq!(conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}")), tag);
    }
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert_eq!(error_code(&mut conn, "INSERT INTO k VALUES (101, 101, 101)"), "55000");
    assert_eq!(error_code(&mut conn, "REINDEX INDEX k_a"), "22012");
    for sql in [compare, "REINDEX TABLE k", "INSERT INTO k VALUES (101, 101, 101)"] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    assert!(value(&mut conn, "EXPLAIN (COSTS OFF) SELECT a FROM k WHERE a = 5").contains("using k_a on k"));
    assert_eq!(value(&mut conn, "SELECT count(*) FROM k WHERE a > 50"), "51");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM k WHERE b = 200"), "1");
    assert_eq!(error_code(&mut conn, "INSERT INTO k VALUES (102, 101, 0)"), "23505");
    conn.close();
}

#[test]
fn test_comparator_indexes_of_each_database_call_its_own_comparison_function() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CREATE DATABASE d2", &[]).unwrap();
    conn.close();
    let set_up = [
        "CREATE FUNCTION my_cmp(int4, int4) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$",
        "CREATE OPERATOR CLASS my_ops FOR TYPE int4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 my_cmp(int4, int4)",
        "CREATE TABLE k (id int PRIMARY KEY, a int)",
        "INSERT INTO k SELECT i, i FROM generate_series(1, 10) i",
        "CREATE UNIQUE INDEX k_a ON k (a my_ops)",
        "SET enable_seqscan = off",
    ];
    let mut conns = ["postgres", "d2"].map(|database| connect(&server, database, "postgres"));
    for conn in &mut conns {
        for sql in set_up {
            conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
        }
    }
    let raise = "CREATE OR REPLACE FUNCTION my_cmp(int4, int4) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT $1 / ($2 - $2) $$";
    conns[1].exec(raise, &[]).unwrap();
    assert_eq!(error_code(&mut conns[1], "INSERT INTO k VALUES (11, 11)"), "22012");
    assert_eq!(conns[0].exec("INSERT INTO k VALUES (11, 11)", &[]).unwrap(), "INSERT 0 1");
    assert_eq!(value(&mut conns[0], "SELECT a FROM k WHERE a = 11"), "11");
    assert_eq!(error_code(&mut conns[1], "SELECT a FROM k WHERE a = 5"), "22012");
    for mut conn in conns {
        conn.close();
    }
}

/// indexed returns a query's one value read with sequential scans off, after checking that its plan reads an index.
fn indexed(conn: &mut Conn, sql: &str) -> String {
    run(conn, &["SET enable_seqscan = off"]);
    let plan = conn.simple_query_rows(&format!("EXPLAIN (COSTS OFF) {sql}")).unwrap_or_else(|err| panic!("{sql}: {err}"));
    let plan: Vec<String> = plan.rows.iter().map(|row| String::from_utf8(row[0].clone().unwrap_or_default()).unwrap()).collect();
    assert!(plan.iter().any(|line| line.contains("Index")), "{sql}: {plan:?}");
    value(conn, sql)
}

#[test]
fn test_recovery_of_a_table_dropped_and_created_again_under_its_name() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE pk (a INT, b INT, c INT, d TEXT, PRIMARY KEY (a, b, c))",
        "CREATE INDEX pk_d ON pk (d)",
        "INSERT INTO pk SELECT i, i, i, 'v' || i FROM generate_series(1, 100) i",
        "CHECKPOINT",
        "INSERT INTO pk SELECT i, i, i, 'v' || i FROM generate_series(101, 110) i",
        "DROP TABLE pk",
        "CREATE TABLE pk (x INT, y INT, z INT, PRIMARY KEY (x, y))",
        "CREATE INDEX pk_z ON pk (z)",
        "INSERT INTO pk SELECT i, 0, i FROM generate_series(1, 100) i",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM pk WHERE z < 50"), "49");
    conn.close();
}

#[test]
fn test_recovery_of_a_table_created_and_written_in_one_transaction() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE loaded (id INT PRIMARY KEY, a INT)",
        "CHECKPOINT",
        "INSERT INTO loaded VALUES (1, 1)",
        "BEGIN",
        "CREATE TABLE c (id INT PRIMARY KEY, a INT)",
        "CREATE INDEX c_a ON c (a)",
        "INSERT INTO c SELECT i, i FROM generate_series(1, 100) i",
        "COMMIT",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM c WHERE a < 50"), "49");
    conn.close();
}

#[test]
fn test_recovery_of_columns_changed_and_written_in_one_transaction() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE t (id INT PRIMARY KEY, x INT, b INT)",
        "CREATE INDEX t_b ON t (b)",
        "INSERT INTO t SELECT i, -i, i FROM generate_series(1, 50) i",
        "CHECKPOINT",
        "INSERT INTO t SELECT i, -i, i FROM generate_series(51, 60) i",
        "BEGIN",
        "ALTER TABLE t DROP COLUMN x",
        "ALTER TABLE t ADD COLUMN c INT",
        "INSERT INTO t SELECT i, i, i * 10 FROM generate_series(61, 100) i",
        "COMMIT",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM t WHERE b > 55"), "45");
    assert_eq!(indexed(&mut conn, "SELECT c FROM t WHERE b = 70"), "700");
    conn.close();
}

#[test]
fn test_recovery_of_rows_written_around_a_rewrite_in_one_transaction() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE w (id INT PRIMARY KEY, b INT)",
        "CREATE INDEX w_b ON w (b)",
        "INSERT INTO w SELECT i, i FROM generate_series(1, 10) i",
        "CHECKPOINT",
        "BEGIN",
        "INSERT INTO w VALUES (11, 11)",
        "ALTER TABLE w ALTER COLUMN b TYPE BIGINT",
        "INSERT INTO w SELECT i, i FROM generate_series(12, 20) i",
        "COMMIT",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM w WHERE b > 0"), "20");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM w WHERE b = 11"), "1");
    conn.close();
}

#[test]
fn test_recovery_of_a_table_dropped_and_created_again_in_one_transaction() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE swapped (a INT, b INT, c TEXT, PRIMARY KEY (a, b))",
        "CREATE INDEX swapped_c ON swapped (c)",
        "INSERT INTO swapped SELECT i, i, 'v' || i FROM generate_series(1, 100) i",
        "CHECKPOINT",
        "INSERT INTO swapped VALUES (101, 101, 'v101')",
        "BEGIN",
        "DROP TABLE swapped",
        "CREATE TABLE swapped (x TEXT PRIMARY KEY, y INT)",
        "CREATE INDEX swapped_y ON swapped (y)",
        "INSERT INTO swapped SELECT 'k' || i, i FROM generate_series(1, 100) i",
        "COMMIT",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM swapped WHERE y < 50"), "49");
    conn.close();
}

/// test_a_copied_database_keeps_its_role_memberships_when_its_template_changes_them is Doltgrust's own behavior: roles
/// are per database with behavior.server_wide_roles off; Postgres' roles are cluster-wide.
#[test]
fn test_a_copied_database_keeps_its_role_memberships_when_its_template_changes_them() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "behavior:\n  server_wide_roles: false\n").unwrap();
    let mut conn = connect(&server, "template1", "postgres");
    for sql in ["CREATE ROLE ro_a", "CREATE ROLE ro_b", "GRANT ro_a TO ro_b"] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let role = "SELECT oid FROM pg_authid WHERE rolname = 'ro_a'";
    let member = "SELECT oid FROM pg_auth_members WHERE member = 'ro_b'::regrole";
    let (a, membership) = (value(&mut conn, role), value(&mut conn, member));
    conn.exec("CREATE DATABASE ro_copy", &[]).unwrap();
    let old_b = value(&mut conn, "SELECT oid FROM pg_authid WHERE rolname = 'ro_b'");
    conn.exec("DROP ROLE ro_b", &[]).unwrap();
    conn.exec("CREATE ROLE ro_b", &[]).unwrap();
    let b = value(&mut conn, "SELECT oid FROM pg_authid WHERE rolname = 'ro_b'");
    assert_ne!(b, old_b);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "template1", "postgres");
    assert_eq!(value(&mut conn, "SELECT oid FROM pg_authid WHERE rolname = 'ro_b'"), b);
    assert_eq!(value(&mut conn, "SELECT count(*) FROM pg_auth_members WHERE member = 'ro_b'::regrole"), "0");
    conn.close();
    let mut conn = connect(&server, "ro_copy", "postgres");
    assert_eq!(value(&mut conn, role), a);
    assert_eq!(value(&mut conn, member), membership);
    conn.close();
}

/// test_indexes_whose_recovery_build_fails_stay_broken_until_a_reindex is Doltgrust's own behavior: an index whose
/// function fails while recovery builds it again is left for REINDEX with a WARNING, where Postgres' WAL holds the
/// index's entries and needs no build.
#[test]
fn test_indexes_whose_recovery_build_fails_stay_broken_until_a_reindex() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE flag (x INT)",
        "CREATE FUNCTION f(i INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN IF (SELECT count(*) FROM public.flag) > 0 THEN RAISE EXCEPTION 'flag is set'; END IF; RETURN i; END $$",
        "CREATE TABLE t (id INT PRIMARY KEY, v INT)",
        "CREATE INDEX t_f ON t (f(v))",
        "CHECKPOINT",
        "INSERT INTO t SELECT i, i FROM generate_series(1, 10) i",
        "INSERT INTO flag VALUES (1)",
    ]);
    conn.close();
    server.restart().unwrap();
    let log = std::fs::read_to_string(server.directory().join("server.log")).unwrap();
    assert!(log.contains("index \"t_f\" is invalid until it is reindexed: building it during recovery failed: flag is set"), "{log}");
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert!(value(&mut conn, "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE f(v) = 3").starts_with("Seq Scan on t"));
    assert_eq!(value(&mut conn, "SELECT count(*) FROM t"), "10");
    assert_eq!(error_code(&mut conn, "REINDEX INDEX t_f"), "P0001");
    run(&mut conn, &["DELETE FROM flag", "REINDEX INDEX t_f"]);
    assert!(value(&mut conn, "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE f(v) = 3").contains("using t_f on t"));
    assert_eq!(indexed(&mut conn, "SELECT id FROM t WHERE f(v) = 3"), "3");
    conn.close();
}

#[test]
fn test_a_thousand_tables_created_two_hundred_at_a_time_with_checkpoints_survive_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for step in 0..5 {
        let create = format!(
            "DO $$ BEGIN FOR i IN {} .. {} LOOP EXECUTE format('CREATE TABLE many_%s (a INT)', i); EXECUTE format('INSERT INTO many_%s VALUES (%s)', i, i); END LOOP; END $$",
            step * 200,
            step * 200 + 199
        );
        run(&mut conn, &[&create, "CHECKPOINT"]);
    }
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for i in 0..1000 {
        assert_eq!(value(&mut conn, &format!("SELECT a FROM many_{i}")), i.to_string());
    }
    conn.close();
}

#[test]
fn test_user_ranges_survive_a_restart_and_a_checkpoint() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')",
        "CREATE TYPE moodrange AS RANGE (subtype = mood)",
        "CREATE TYPE myrange AS RANGE (subtype = int4)",
        "CREATE TABLE t (id INT PRIMARY KEY, mr moodrange, r myrange, rs myrange[])",
        "CREATE INDEX t_mr ON t (mr)",
        "INSERT INTO t VALUES (1, '[sad,happy]', '[1,5)', ARRAY['[2,3)'::myrange]), (2, '(ok,)', 'empty', NULL)",
        "CHECKPOINT",
        "INSERT INTO t VALUES (3, '[sad,ok)', '(,7]', ARRAY['empty'::myrange])",
    ]);
    let rows = "SELECT id, mr, r, rs FROM t ORDER BY id";
    let all = |conn: &mut Conn| {
        let result = conn.simple_query_rows(rows).unwrap_or_else(|err| panic!("{rows}: {err}"));
        result.rows.iter().map(|row| row.iter().map(|v| String::from_utf8(v.clone().unwrap_or_default()).unwrap()).collect::<Vec<_>>().join("|")).collect::<Vec<_>>()
    };
    let expected = vec!["1|[sad,happy]|[1,5)|{\"[2,3)\"}", "2|(ok,)|empty|", "3|[sad,ok)|(,7]|{empty}"];
    assert_eq!(all(&mut conn), expected);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(all(&mut conn), expected);
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert_eq!(value(&mut conn, "SELECT id FROM t WHERE mr = '[sad,ok)'"), "3");
    assert_eq!(value(&mut conn, "SELECT string_agg(id::text, ',' ORDER BY mr) FROM t"), "3,1,2");
    conn.close();
}

#[test]
fn test_user_multiranges_survive_a_restart_and_a_checkpoint() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')",
        "CREATE TYPE moodrange AS RANGE (subtype = mood, multirange_type_name = moods)",
        "CREATE TABLE t (id INT PRIMARY KEY, m moods)",
        "CREATE INDEX t_m ON t (m)",
        "INSERT INTO t VALUES (1, '{[sad,sad], [happy,)}'), (2, '{}')",
        "CHECKPOINT",
        "INSERT INTO t VALUES (3, '{(sad,ok]}')",
    ]);
    let rows = "SELECT string_agg(id || '=' || m, ' ' ORDER BY id) FROM t";
    let expected = "1={[sad,sad],[happy,)} 2={} 3={(sad,ok]}";
    assert_eq!(value(&mut conn, rows), expected);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, rows), expected);
    conn.exec("SET enable_seqscan = off", &[]).unwrap();
    assert_eq!(value(&mut conn, "SELECT id FROM t WHERE m = '{(sad,ok]}'"), "3");
    assert_eq!(value(&mut conn, "SELECT string_agg(id::text, ',' ORDER BY m) FROM t"), "2,1,3");
    conn.close();
}

#[test]
fn test_a_table_created_in_a_tablespace_keeps_its_rows_there() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let location = server.directory().join("fast");
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    for sql in [
        create.as_str(),
        "CREATE TABLE far (a int) TABLESPACE fast",
        "INSERT INTO far SELECT generate_series(1, 100)",
        "CHECKPOINT",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let files = |dir: &std::path::Path| -> Vec<std::path::PathBuf> {
        let mut found = Vec::new();
        let mut dirs = vec![dir.to_path_buf()];
        while let Some(dir) = dirs.pop() {
            for entry in std::fs::read_dir(&dir).unwrap() {
                let path = entry.unwrap().path();
                match path.is_dir() {
                    true => dirs.push(path),
                    false => found.push(path),
                }
            }
        }
        found
    };
    let placed = match target.is_postgres() {
        true => {
            let file = value(&mut conn, "SELECT pg_relation_filepath('far')");
            let relative = file.strip_prefix("pg_tblspc/").and_then(|rest| rest.split_once('/')).unwrap().1.to_string();
            files(&location).iter().any(|path| path.ends_with(&relative))
        }
        false => files(&location).iter().any(|path| path.extension().is_some_and(|ext| ext == "pack")),
    };
    assert!(placed, "{:?}", files(&location));
    conn.exec("INSERT INTO far SELECT generate_series(101, 150)", &[]).unwrap();
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM far"), "150");
    conn.close();
}

#[test]
fn test_tables_of_a_database_in_a_tablespace_come_back_after_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let location = server.directory().join("fast");
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    for sql in [create.as_str(), "CREATE DATABASE d TABLESPACE fast"] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    let mut conn = connect(&server, "d", "postgres");
    for sql in [
        "CREATE TABLE home (a int) TABLESPACE pg_default",
        "CREATE TABLE own (a int)",
        "INSERT INTO home SELECT generate_series(1, 100)",
        "INSERT INTO own SELECT generate_series(1, 100)",
        "CHECKPOINT",
        "INSERT INTO home SELECT generate_series(101, 150)",
        "INSERT INTO own SELECT generate_series(101, 150)",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "d", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM home"), "150");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM own"), "150");
    conn.close();
}

#[test]
fn test_a_table_in_a_tablespace_whose_location_is_missing_fails_until_it_is_back() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let (location, moved) = (server.directory().join("fast"), server.directory().join("moved"));
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    for sql in [
        create.as_str(),
        "CREATE TABLE far (a int) TABLESPACE fast",
        "INSERT INTO far SELECT generate_series(1, 100)",
        "CREATE TABLE near (a int)",
        "INSERT INTO near VALUES (1)",
        "CHECKPOINT",
        "INSERT INTO far SELECT generate_series(101, 150)",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    server.halt();
    std::fs::rename(&location, &moved).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(error_code(&mut conn, "SELECT count(*) FROM far"), "58P01");
    assert_eq!(error_code(&mut conn, "INSERT INTO far VALUES (151)"), "58P01");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM near"), "1");
    conn.exec("CHECKPOINT", &[]).unwrap();
    conn.close();
    server.halt();
    std::fs::rename(&moved, &location).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM far"), "150");
    conn.close();
}

#[test]
fn test_a_database_whose_tablespace_location_is_missing_is_unavailable_and_gets_role_changes_once_it_is_back() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let (location, moved) = (server.directory().join("fast"), server.directory().join("moved"));
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    run(&mut conn, &[create.as_str(), "CREATE DATABASE shop TABLESPACE fast"]);
    let oid = value(&mut conn, "SELECT oid FROM pg_tablespace WHERE spcname = 'fast'");
    conn.close();
    let mut shop = connect(&server, "shop", "postgres");
    run(&mut shop, &["CREATE TABLE t (a INT)", "INSERT INTO t SELECT generate_series(1, 20)"]);
    shop.close();
    server.halt();
    std::fs::rename(&location, &moved).unwrap();
    server.restart().unwrap();
    let url = format!("postgres://postgres:password@127.0.0.1:{}/shop?sslmode=disable", server.port);
    let refused = Conn::connect(harness::pgx::ConnConfig::parse(&url).unwrap()).err().unwrap();
    let fields = &refused.pg_error().unwrap_or_else(|| panic!("{refused}")).fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("3D000", "database \"shop\" does not exist"));
    let subdirectory = format!("The database subdirectory \"pg_tblspc/{oid}/");
    assert!(fields.detail.starts_with(&subdirectory) && fields.detail.ends_with("\" is missing."), "{}", fields.detail);
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["CREATE ROLE deferred_role", "CHECKPOINT"]);
    conn.close();
    server.halt();
    std::fs::rename(&moved, &location).unwrap();
    server.restart().unwrap();
    let mut shop = connect(&server, "shop", "postgres");
    assert_eq!(value(&mut shop, "SELECT count(*) FROM t"), "20");
    assert_eq!(value(&mut shop, "SELECT count(*) FROM pg_roles WHERE rolname = 'deferred_role'"), "1");
    shop.close();
}

#[test]
fn test_an_unavailable_database_can_be_renamed_altered_and_dropped() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let location = server.directory().join("fast");
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    run(&mut conn, &[create.as_str(), "CREATE DATABASE shop TABLESPACE fast", "CREATE ROLE shop_owner"]);
    let oid = value(&mut conn, "SELECT oid FROM pg_tablespace WHERE spcname = 'fast'");
    conn.close();
    let mut shop = connect(&server, "shop", "postgres");
    run(&mut shop, &["CREATE TABLE t (a INT)", "INSERT INTO t SELECT generate_series(1, 20)"]);
    shop.close();
    server.halt();
    std::fs::remove_dir_all(&location).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let shown = "SELECT pg_get_userbyid(datdba) || ' ' || (datacl IS NULL) FROM pg_database WHERE datname = 'shop'";
    assert_eq!(value(&mut conn, shown), "postgres true");
    let (code, message, _) = error(&mut conn, "CREATE DATABASE copied TEMPLATE shop");
    let missing = format!("could not open file \"pg_tblspc/{oid}/DOLTGRUST/");
    assert!(code == "58P01" && message.starts_with(&missing), "{code} {message}");
    run(&mut conn, &["ALTER DATABASE shop RENAME TO shop2", "ALTER DATABASE shop2 CONNECTION LIMIT 5"]);
    let row = "SELECT datname || ' ' || datconnlimit FROM pg_database WHERE oid = (SELECT max(oid) FROM pg_database)";
    assert_eq!(value(&mut conn, row), "shop2 5");
    for unreadable in ["ALTER DATABASE shop2 SET work_mem = '8MB'", "ALTER DATABASE shop2 OWNER TO shop_owner"] {
        assert_eq!(error(&mut conn, unreadable).0, "58P01", "{unreadable}");
    }
    let url = format!("postgres://postgres:password@127.0.0.1:{}/shop2?sslmode=disable", server.port);
    let refused = Conn::connect(harness::pgx::ConnConfig::parse(&url).unwrap()).err().unwrap();
    let fields = &refused.pg_error().unwrap_or_else(|| panic!("{refused}")).fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("3D000", "database \"shop2\" does not exist"));
    run(&mut conn, &["DROP DATABASE shop2"]);
    assert_eq!(value(&mut conn, "SELECT count(*) FROM pg_database WHERE datname LIKE 'shop%'"), "0");
    run(&mut conn, &["CREATE DATABASE shop"]);
    conn.close();
    server.halt();
    server.restart().unwrap();
    let mut shop = connect(&server, "shop", "postgres");
    assert_eq!(value(&mut shop, "SELECT count(*) FROM pg_class WHERE relname = 't'"), "0");
    shop.close();
}

#[test]
fn test_a_checkpoint_after_inserts_that_drew_from_a_sequence_leaves_no_delta_and_the_rows_survive_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    for sql in [
        "CREATE TABLE s (id SERIAL, k INTEGER NOT NULL, PRIMARY KEY (id))",
        "INSERT INTO s (k) SELECT i FROM generate_series(1, 10000) AS i",
        "CHECKPOINT",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let delta = "SELECT allocated_size FROM pg_shmem_allocations WHERE name = 'Delta'";
    assert_eq!(value(&mut conn, delta), "0");
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM s"), "10000");
    assert_eq!(value(&mut conn, "SELECT max(id) FROM s"), "10000");
    conn.exec("CHECKPOINT", &[]).unwrap();
    assert_eq!(value(&mut conn, delta), "0");
    conn.close();
}

#[test]
fn test_rows_and_sizes_of_tables_without_secondary_indexes_survive_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE keyed (id int PRIMARY KEY, v text)",
            "CREATE TABLE keyless (id int, v text)",
            "INSERT INTO keyed SELECT i, repeat(md5(i::text), 400) FROM generate_series(1, 200) i",
            "INSERT INTO keyless SELECT i % 50, repeat(md5(i::text), 400) FROM generate_series(1, 200) i",
            "UPDATE keyed SET v = repeat(md5((id * 7)::text), 300) WHERE id % 3 = 0",
            "DELETE FROM keyed WHERE id % 5 = 0",
            "UPDATE keyless SET v = 'short' WHERE id % 4 = 0",
            "DELETE FROM keyless WHERE id % 7 = 0",
        ],
    );
    let state = |conn: &mut Conn| {
        let keyed = rows(conn, "SELECT id, md5(v) FROM keyed ORDER BY id");
        let keyless = rows(conn, "SELECT id, md5(v), count(*) FROM keyless GROUP BY id, v ORDER BY id, 2");
        let sizes = rows(
            conn,
            "SELECT relname, pg_relation_size(oid), pg_total_relation_size(oid), relpages FROM pg_class WHERE relname IN ('keyed', 'keyless') ORDER BY relname",
        );
        (keyed, keyless, sizes)
    };
    let before = state(&mut conn);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(state(&mut conn), before);
    conn.close();
}

/// ALL_SIZES are the sizes of a relation the restart tests compare: its main fork's, its total, and its pages.
const ALL_SIZES: &str = "pg_relation_size(oid), pg_total_relation_size(oid), relpages";

/// rows_and_sizes returns each table's rows in order and the sizes `pg_class` and the size functions give the tables.
fn rows_and_sizes(conn: &mut Conn, tables: &[&str], sizes: &str) -> Vec<Vec<Vec<String>>> {
    let mut state: Vec<_> = tables.iter().map(|table| rows(conn, &format!("SELECT * FROM {table} ORDER BY 1, 2"))).collect();
    let names = tables.iter().map(|table| format!("'{table}'")).collect::<Vec<_>>().join(", ");
    let sizes = format!("SELECT relname, {sizes} FROM pg_class WHERE relname IN ({names}) ORDER BY relname");
    state.push(rows(conn, &sizes));
    state
}

#[test]
fn test_a_keyless_load_that_a_later_primary_key_rewrites_survives_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE accounts (aid INT, abalance INT, filler TEXT)",
            "BEGIN",
            "TRUNCATE accounts",
            "INSERT INTO accounts SELECT i, 0, repeat('x', 80) FROM generate_series(1, 2000) i",
            "COMMIT",
            "ALTER TABLE accounts ADD PRIMARY KEY (aid)",
            "UPDATE accounts SET abalance = abalance + aid WHERE aid % 7 = 0",
            "CREATE TABLE history (aid INT, delta INT)",
            "INSERT INTO history SELECT i, i * 2 FROM generate_series(1, 300) i",
            "DELETE FROM accounts WHERE aid % 11 = 0",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["accounts", "history"], ALL_SIZES);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["accounts", "history"], ALL_SIZES), before);
    conn.close();
}

#[test]
fn test_a_load_whose_rewrites_rolled_back_or_never_committed_survives_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE loaded (a INT, b TEXT)",
            "INSERT INTO loaded SELECT i, md5(i::text) FROM generate_series(1, 500) i",
            "BEGIN",
            "ALTER TABLE loaded ADD PRIMARY KEY (a)",
            "ROLLBACK",
            "CREATE TABLE other (a INT)",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["loaded"], ALL_SIZES);
    let mut open = connect(&server, "postgres", "postgres");
    run(&mut open, &["BEGIN", "TRUNCATE loaded"]);
    run(&mut conn, &["INSERT INTO other VALUES (1)"]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["loaded"], ALL_SIZES), before);
    conn.close();
}

/// This leaves out `pg_total_relation_size`, whose free space map, visibility map, and index sizes Postgres doesn't keep
/// byte for byte through crash recovery after a rewrite.
#[test]
fn test_a_load_rewritten_twice_survives_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE twice (a INT, b TEXT)",
            "INSERT INTO twice SELECT i, md5(i::text) FROM generate_series(1, 500) i",
            "ALTER TABLE twice ADD PRIMARY KEY (a)",
            "INSERT INTO twice SELECT i, 'between' FROM generate_series(501, 600) i",
            "UPDATE twice SET b = 'updated' WHERE a % 5 = 0",
            "ALTER TABLE twice ALTER COLUMN a TYPE BIGINT",
            "INSERT INTO twice SELECT i, 'after' FROM generate_series(601, 650) i",
            "DELETE FROM twice WHERE a % 9 = 0",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["twice"], "pg_relation_size(oid), relpages");
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["twice"], "pg_relation_size(oid), relpages"), before);
    conn.close();
}

/// Postgres refuses to start while a tablespace's location is missing (its recovery fails redoing the tablespace's
/// creation), where Doltgrust starts and replays the tablespace's changes once a restart finds the location again.
#[test]
fn test_a_load_rewritten_into_a_tablespace_missing_at_a_crash_comes_back_with_it() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let (location, moved) = (server.directory().join("fast"), server.directory().join("moved"));
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    run(
        &mut conn,
        &[
            create.as_str(),
            "CREATE TABLE far (a INT, b TEXT)",
            "INSERT INTO far SELECT i, md5(i::text) FROM generate_series(1, 300) i",
            "ALTER TABLE far SET TABLESPACE fast",
            "ALTER TABLE far ADD PRIMARY KEY (a)",
            "INSERT INTO far SELECT i, 'after' FROM generate_series(301, 350) i",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["far"], ALL_SIZES);
    conn.close();
    server.halt();
    std::fs::rename(&location, &moved).unwrap();
    server.restart().unwrap();
    server.halt();
    std::fs::rename(&moved, &location).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["far"], ALL_SIZES), before);
    conn.close();
}

#[test]
fn test_a_prepared_transaction_over_a_rewritten_load_survives_a_crash() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start_with_prepared_transactions(&target).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE held (a INT, b TEXT)",
            "INSERT INTO held SELECT i, md5(i::text) FROM generate_series(1, 400) i",
            "ALTER TABLE held ADD PRIMARY KEY (a)",
            "BEGIN",
            "INSERT INTO held SELECT i, 'prepared' FROM generate_series(401, 420) i",
            "UPDATE held SET b = 'prepared' WHERE a % 10 = 0",
            "DELETE FROM held WHERE a % 13 = 0",
            "PREPARE TRANSACTION 'held'",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["held"], ALL_SIZES);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["held"], ALL_SIZES), before);
    run(&mut conn, &["COMMIT PREPARED 'held'"]);
    assert_eq!(value(&mut conn, "SELECT count(*) FROM held WHERE b = 'prepared'"), "55");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM held"), "388");
    conn.close();
}

#[test]
fn test_planned_sizes_of_loaded_and_rewritten_tables_are_the_same_after_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE loaded (a INT PRIMARY KEY, b TEXT)",
            "INSERT INTO loaded SELECT i, repeat('x', i % 50) FROM generate_series(1, 3000) i",
            "CREATE INDEX loaded_b ON loaded (b)",
            "CREATE TABLE rewritten (a INT PRIMARY KEY, b INT)",
            "INSERT INTO rewritten SELECT i, i FROM generate_series(1, 2000) i",
            "ALTER TABLE rewritten ALTER COLUMN b TYPE BIGINT",
            "VACUUM loaded",
            "VACUUM rewritten",
            "CHECKPOINT",
        ],
    );
    let planned = |conn: &mut Conn| {
        let names = "'loaded', 'loaded_pkey', 'loaded_b', 'rewritten', 'rewritten_pkey'";
        let mut state = vec![rows(
            conn,
            &format!("SELECT relname, {ALL_SIZES}, reltuples FROM pg_class WHERE relname IN ({names}) ORDER BY relname"),
        )];
        state.push(rows(conn, "EXPLAIN SELECT * FROM loaded WHERE b > 'xxxxx'"));
        state.push(rows(conn, "EXPLAIN SELECT * FROM rewritten WHERE b < 500"));
        state
    };
    let before = planned(&mut conn);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(planned(&mut conn), before);
    conn.close();
}

/// A commit or a branch would copy a table recovery holds without the rows only the WAL keeps, so both fail as reads
/// of the table do until its tablespace's location is back.
#[test]
fn test_a_commit_or_a_branch_of_a_table_whose_tablespace_location_is_missing_fails_until_it_is_back() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let (location, moved) = (server.directory().join("fast"), server.directory().join("moved"));
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    for sql in [
        create.as_str(),
        "CREATE TABLE far (a INT PRIMARY KEY) TABLESPACE fast",
        "INSERT INTO far SELECT generate_series(1, 100)",
        "SELECT dolt_commit('-Am', 'first')",
        "CHECKPOINT",
        "INSERT INTO far SELECT generate_series(101, 150)",
    ] {
        conn.exec(sql, &[]).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    conn.close();
    server.halt();
    std::fs::rename(&location, &moved).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(error_code(&mut conn, "SELECT dolt_commit('-Am', 'held')"), "58P01");
    assert_eq!(error_code(&mut conn, "SELECT dolt_branch('copy')"), "58P01");
    conn.close();
    server.halt();
    std::fs::rename(&moved, &location).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM far"), "150");
    conn.exec("SELECT dolt_commit('-Am', 'back')", &[]).unwrap();
    conn.close();
}

/// Rewrites, truncations, and moves of tables into a tablespace whose location is missing at the next start wait in
/// the WAL through a checkpoint taken then, and come back whole with the location.
#[test]
fn test_rewrites_and_truncates_of_tables_in_a_tablespace_missing_at_a_checkpoint_come_back_with_it() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let (location, moved) = (server.directory().join("fast"), server.directory().join("moved"));
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    run(
        &mut conn,
        &[
            create.as_str(),
            "CREATE TABLE cut (a INT PRIMARY KEY, b TEXT) TABLESPACE fast",
            "INSERT INTO cut SELECT i, md5(i::text) FROM generate_series(1, 100) i",
            "CREATE TABLE typed (a INT PRIMARY KEY, b INT) TABLESPACE fast",
            "INSERT INTO typed SELECT i, i FROM generate_series(1, 100) i",
            "CHECKPOINT",
            "CREATE TABLE far (a INT, b TEXT)",
            "INSERT INTO far SELECT i, md5(i::text) FROM generate_series(1, 300) i",
            "ALTER TABLE far SET TABLESPACE fast",
            "ALTER TABLE far ADD PRIMARY KEY (a)",
            "INSERT INTO far SELECT i, 'after' FROM generate_series(301, 350) i",
            "TRUNCATE cut",
            "INSERT INTO cut SELECT i, 'again' FROM generate_series(1, 20) i",
            "ALTER TABLE typed ALTER COLUMN b TYPE BIGINT",
            "UPDATE typed SET b = b * 1000000000 WHERE a % 2 = 0",
        ],
    );
    let before = rows_and_sizes(&mut conn, &["cut", "far", "typed"], ALL_SIZES);
    conn.close();
    server.halt();
    std::fs::rename(&location, &moved).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    conn.exec("CHECKPOINT", &[]).unwrap();
    conn.close();
    server.halt();
    std::fs::rename(&moved, &location).unwrap();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows_and_sizes(&mut conn, &["cut", "far", "typed"], ALL_SIZES), before);
    conn.close();
}

#[test]
fn test_vacuum_counts_a_keyless_tables_rows_from_its_kept_sizes_after_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE copies (a INT, b TEXT)",
            "INSERT INTO copies SELECT i % 100, 'same' FROM generate_series(1, 3000) i",
            "VACUUM copies",
            "CHECKPOINT",
        ],
    );
    let reltuples = "SELECT reltuples::bigint FROM pg_class WHERE relname = 'copies'";
    assert_eq!(value(&mut conn, reltuples), "3000");
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["DELETE FROM copies WHERE a < 10", "INSERT INTO copies VALUES (1, 'same'), (1, 'same')", "VACUUM copies"]);
    assert_eq!(value(&mut conn, reltuples), "2702");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM copies"), "2702");
    conn.close();
}

/// A table without a primary key whose row was written before it was indexed and deleted after comes back from a crash
/// once a DROP or a TRUNCATE replaced its tree, with its index matching it.
#[test]
fn test_recovery_of_a_keyless_table_indexed_between_a_write_and_a_delete_then_dropped_or_truncated() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE d (i INT, x TEXT)",
        "INSERT INTO d VALUES (1, 'a')",
        "CREATE INDEX d_i ON d (i)",
        "DELETE FROM d WHERE i = 1",
        "DROP TABLE d",
        "CREATE TABLE t (i INT, x TEXT)",
        "INSERT INTO t VALUES (1, 'a'), (2, 'b')",
        "CREATE INDEX t_i ON t (i)",
        "DELETE FROM t WHERE i = 1",
        "TRUNCATE t",
        "INSERT INTO t VALUES (3, 'c')",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM pg_class WHERE relname = 'd'"), "0");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM t WHERE i > 0"), "1");
    assert_eq!(value(&mut conn, "SELECT string_agg(x, ',') FROM t"), "c");
    conn.close();
}

/// A row of a table without a primary key written before a TRUNCATE stays truncated after a crash, when an index made
/// after the TRUNCATE derives the rows written after it until a later TRUNCATE.
#[test]
fn test_recovery_of_a_keyless_table_truncated_before_it_was_indexed_and_truncated_again() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE t (i INT, x TEXT)",
        "INSERT INTO t VALUES (1, 'a')",
        "TRUNCATE t",
        "CREATE INDEX t_x ON t (x)",
        "INSERT INTO t VALUES (2, 'a')",
        "INSERT INTO t VALUES (3, 'b')",
        "DELETE FROM t WHERE i = 3",
        "TRUNCATE t",
        "INSERT INTO t VALUES (4, 'c')",
    ]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM t"), "1");
    assert_eq!(indexed(&mut conn, "SELECT count(*) FROM t WHERE x >= 'a'"), "1");
    assert_eq!(value(&mut conn, "SELECT string_agg(x, ',') FROM t"), "c");
    conn.close();
}

/// A table without a primary key that a unique index failed on between a write and a delete, as Postgres' `unique_tbl`
/// regression run does, comes back from a crash after its drop.
#[test]
fn test_recovery_of_a_keyless_table_a_unique_index_failed_on_then_dropped() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE u (i INT, t TEXT)",
        "INSERT INTO u VALUES (1, 'one'), (2, 'two'), (2, 'two again')",
        "CREATE UNIQUE INDEX u_t ON u (t)",
    ]);
    assert_eq!(error_code(&mut conn, "CREATE UNIQUE INDEX u_i ON u (i)"), "23505");
    run(&mut conn, &["DELETE FROM u WHERE t = 'two again'", "CREATE UNIQUE INDEX u_i ON u (i)", "DROP TABLE u"]);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM pg_class WHERE relname IN ('u', 'u_t', 'u_i')"), "0");
    run(&mut conn, &["CREATE TABLE u (i INT)", "INSERT INTO u VALUES (1)"]);
    assert_eq!(value(&mut conn, "SELECT count(*) FROM u"), "1");
    conn.close();
}

/// Rows an UPDATE stores again with the same bytes and new tuple ids keep distinct ctids through a full collection, a
/// restart, and a new row.
#[test]
fn test_rows_stored_again_with_the_same_bytes_keep_distinct_ctids_through_a_collection_and_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &[
        "CREATE TABLE t (id INT PRIMARY KEY, x INT)",
        "INSERT INTO t SELECT i, i % 7 FROM generate_series(1, 20000) i",
        "CHECKPOINT",
        "UPDATE t SET x = x",
        "CHECKPOINT",
        "SELECT dolt_gc()",
        "UPDATE t SET x = x WHERE id % 2 = 0",
        "CHECKPOINT",
    ]);
    let distinct = "SELECT count(*) = count(DISTINCT ctid) FROM t";
    assert_eq!(value(&mut conn, distinct), "t");
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["INSERT INTO t VALUES (20001, 0)"]);
    assert_eq!(value(&mut conn, distinct), "t");
    assert_eq!(value(&mut conn, "SELECT count(*) FROM t"), "20001");
    conn.close();
}

/// Planning with no count of a table's rows shows no planning buffers, so a table whose sizes recovery left unknown
/// shows its count in the first plan's.
#[test]
fn test_the_first_plan_after_a_crash_counts_no_table_changed_since_the_last_checkpoint() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE TABLE changed (a INT PRIMARY KEY, b TEXT)",
            "INSERT INTO changed SELECT i, md5(i::text) FROM generate_series(1, 20000) i",
            "CREATE INDEX changed_b ON changed (b)",
            "CHECKPOINT",
            "UPDATE changed SET b = repeat('x', a % 40) WHERE a % 3 = 0",
            "DELETE FROM changed WHERE a % 7 = 0",
            "INSERT INTO changed SELECT i, md5(i::text) FROM generate_series(20001, 21000) i",
        ],
    );
    let sizes = format!("SELECT relname, {ALL_SIZES} FROM pg_class WHERE relname LIKE 'changed%' ORDER BY relname");
    let before = rows(&mut conn, &sizes);
    conn.close();
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let plan = rows(&mut conn, "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF) SELECT count(*) FROM changed WHERE a < 100");
    assert!(!plan.iter().any(|row| row[0] == "Planning:"), "the first plan after a crash counted rows: {plan:?}");
    assert_eq!(rows(&mut conn, &sizes), before);
    conn.close();
}

/// A sort that spills to a temporary tablespace whose location lost the directory CREATE TABLESPACE made spills to the
/// database's own instead, as Postgres falls back, and never makes that directory again.
#[test]
fn test_a_spill_to_a_temporary_tablespace_whose_location_lost_its_directory_goes_to_the_databases_own() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if target.is_postgres() {
        return;
    }
    let server = Server::start(&target, "").unwrap();
    let location = server.directory().join("fast");
    std::fs::create_dir(&location).unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let create = format!("CREATE TABLESPACE fast LOCATION '{}'", location.to_string_lossy().replace('\\', "/"));
    conn.exec(&create, &[]).unwrap();
    std::fs::remove_dir_all(location.join("DOLTGRUST")).unwrap();
    run(&mut conn, &["SET temp_tablespaces = fast", "SET work_mem = '64kB'"]);
    let sorted = "SELECT count(*) FROM (SELECT g FROM generate_series(1, 200000) g ORDER BY g DESC) s";
    assert_eq!(value(&mut conn, sorted), "200000");
    assert!(!location.join("DOLTGRUST").exists(), "a spill made the tablespace's directory again");
    conn.close();
}
