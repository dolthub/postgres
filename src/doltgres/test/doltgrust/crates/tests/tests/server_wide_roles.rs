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

//! Tests that `behavior.server_wide_roles` keeps every database's roles the same, as Postgres' roles are the whole
//! server's, and that a server whose databases' roles differ refuses to start with it.

use harness::conn::{connect, error, expected, rows, run};
use harness::script::testdata_dir;
use harness::server::{Server, Target};

/// set_server_wide_roles writes `behavior.server_wide_roles` into a Doltgrust server's configuration file, which
/// takes effect at its next start.
fn set_server_wide_roles(server: &Server, on: bool) {
    let path = server.directory().join("config.yaml");
    let config = std::fs::read_to_string(&path).unwrap();
    let config = config.replace(&format!("server_wide_roles: {}", !on), &format!("server_wide_roles: {on}"));
    std::fs::write(&path, config).unwrap();
}

#[test]
fn test_differing_roles_refuse_to_start_with_server_wide_roles() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if matches!(target, Target::Postgres { .. }) {
        return;
    }
    let mut server = Server::start(&target, "behavior:\n  server_wide_roles: false\n").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["CREATE ROLE sw_local", "ALTER ROLE postgres SET work_mem = '5MB'"]);
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    run(&mut conn, &["CREATE ROLE sw_template CREATEDB"]);
    conn.close();
    set_server_wide_roles(&server, true);
    let refused = server.restart().unwrap_err();
    for line in [
        "FATAL:  the databases' roles differ, which behavior.server_wide_roles does not allow",
        "DETAIL:  Role \"sw_template\" is in database \"template1\" but not in database \"template0\".\n\
         \tRole \"postgres\" differs in its settings between database \"template1\" and database \"postgres\".\n\
         \tRole \"sw_local\" is in database \"postgres\" but not in database \"template1\".\n\
         \tRole \"sw_template\" is in database \"template1\" but not in database \"postgres\".\n",
        "HINT:  Start the server with behavior.server_wide_roles: false, make the roles match in every database, then \
         turn it back on.",
    ] {
        assert!(refused.contains(line), "{line} is not in {refused}");
    }
    set_server_wide_roles(&server, false);
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["DROP ROLE sw_local", "ALTER ROLE postgres RESET work_mem"]);
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    run(&mut conn, &["DROP ROLE sw_template"]);
    conn.close();
    set_server_wide_roles(&server, true);
    server.restart().unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["CREATE ROLE sw_everywhere"]);
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    run(&mut conn, &["DROP ROLE sw_everywhere"]);
    conn.close();
}

#[test]
fn test_roles_are_the_same_in_every_database() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE swr_user LOGIN PASSWORD 'password'",
            "CREATE ROLE swr_group",
            "GRANT swr_group TO swr_user",
            "ALTER ROLE swr_user SET work_mem = '6MB'",
            "GRANT SET ON PARAMETER log_statement_stats TO swr_user",
            "CREATE DATABASE swr_d1",
        ],
    );
    let oid = rows(&mut conn, "SELECT oid FROM pg_roles WHERE rolname = 'swr_user'").remove(0).remove(0);
    conn.close();

    let mut conn = connect(&server, "swr_d1", "postgres");
    let roles = format!(
        "SELECT rolname, rolcanlogin, oid = {oid} AS same_oid FROM pg_roles WHERE rolname LIKE 'swr\\_%' ORDER BY 1"
    );
    assert_eq!(rows(&mut conn, &roles), expected(&[&["swr_group", "f", "f"], &["swr_user", "t", "t"]]));
    let members = "SELECT r.rolname, m.rolname, g.rolname AS grantor FROM pg_auth_members a JOIN pg_roles r ON r.oid = a.roleid \
                  JOIN pg_roles m ON m.oid = a.member JOIN pg_roles g ON g.oid = a.grantor WHERE r.rolname LIKE 'swr\\_%'";
    assert_eq!(rows(&mut conn, members), expected(&[&["swr_group", "swr_user", "postgres"]]));
    let settings = "SELECT s.setdatabase, s.setconfig FROM pg_db_role_setting s JOIN pg_roles r ON r.oid = s.setrole \
                   WHERE r.rolname = 'swr_user'";
    assert_eq!(rows(&mut conn, settings), expected(&[&["0", "{work_mem=6MB}"]]));
    let parameter = "SELECT parname, paracl FROM pg_parameter_acl WHERE parname = 'log_statement_stats'";
    let granted = expected(&[&["log_statement_stats", "{postgres=sA/postgres,swr_user=s/postgres}"]]);
    assert_eq!(rows(&mut conn, parameter), granted);
    run(
        &mut conn,
        &[
            "CREATE ROLE swr_made_in_d1",
            "ALTER ROLE swr_user CREATEDB",
            "CREATE TABLE swr_t (a int)",
            "ALTER TABLE swr_t OWNER TO swr_user",
            "BEGIN",
            "CREATE ROLE swr_rolled_back",
            "ROLLBACK",
        ],
    );
    conn.close();

    let mut conn = connect(&server, "swr_d1", "swr_user");
    let logged_in = "SELECT current_user, current_setting('work_mem')";
    assert_eq!(rows(&mut conn, logged_in), expected(&[&["swr_user", "6MB"]]));
    conn.close();

    let created = "SELECT rolname, rolcreatedb FROM pg_roles WHERE rolname LIKE 'swr\\_%' ORDER BY 1";
    let everywhere = expected(&[&["swr_group", "f"], &["swr_made_in_d1", "f"], &["swr_user", "t"]]);
    let mut conn = connect(&server, "template1", "postgres");
    assert_eq!(rows(&mut conn, created), everywhere);
    conn.close();

    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows(&mut conn, created), everywhere);
    let depends = (
        "2BP01".to_string(),
        "role \"swr_user\" cannot be dropped because some objects depend on it".to_string(),
        "privileges for parameter log_statement_stats
1 object in database swr_d1".to_string(),
    );
    assert_eq!(error(&mut conn, "DROP ROLE swr_user"), depends);
    conn.close();

    let mut conn = connect(&server, "swr_d1", "postgres");
    run(&mut conn, &["DROP TABLE swr_t"]);
    conn.close();

    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "REVOKE SET ON PARAMETER log_statement_stats FROM swr_user",
            "DROP ROLE swr_user",
            "ALTER ROLE swr_made_in_d1 RENAME TO swr_renamed",
        ],
    );
    conn.close();

    let mut conn = connect(&server, "swr_d1", "postgres");
    let left = "SELECT rolname FROM pg_roles WHERE rolname LIKE 'swr\\_%' ORDER BY 1";
    assert_eq!(rows(&mut conn, left), expected(&[&["swr_group"], &["swr_renamed"]]));
    let orphans = "SELECT count(*) FROM pg_db_role_setting s WHERE s.setrole <> 0 \
                  AND NOT EXISTS (SELECT 1 FROM pg_roles r WHERE r.oid = s.setrole)";
    assert_eq!(rows(&mut conn, orphans), expected(&[&["0"]]));
    conn.close();

    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["DROP DATABASE swr_d1", "DROP ROLE swr_group", "DROP ROLE swr_renamed"]);
    conn.close();
}

#[test]
fn test_role_changes_in_two_databases_wait_for_each_other() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut first = connect(&server, "postgres", "postgres");
    run(&mut first, &["CREATE ROLE swc_role", "BEGIN", "ALTER ROLE swc_role CREATEDB"]);
    let mut second = connect(&server, "template1", "postgres");
    run(&mut second, &["SET lock_timeout = '300ms'"]);
    let timeout = ("55P03".to_string(), "canceling statement due to lock timeout".to_string(), String::new());
    assert_eq!(error(&mut second, "ALTER ROLE swc_role NOCREATEDB"), timeout);
    run(&mut first, &["COMMIT"]);
    run(&mut second, &["ALTER ROLE swc_role CONNECTION LIMIT 3"]);
    let changed = "SELECT rolcreatedb, rolconnlimit FROM pg_roles WHERE rolname = 'swc_role'";
    assert_eq!(rows(&mut first, changed), expected(&[&["t", "3"]]));
    run(&mut second, &["DROP ROLE swc_role"]);
    assert_eq!(rows(&mut first, changed), expected(&[]));
    first.close();
    second.close();
}

#[test]
fn test_role_changes_reach_every_database_after_a_restart() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE swr_kept LOGIN PASSWORD 'password'",
            "CREATE ROLE swr_group",
            "CREATE ROLE swr_dropped",
            "GRANT swr_group TO swr_kept WITH ADMIN OPTION",
            "ALTER ROLE swr_kept SET work_mem = '7MB'",
            "GRANT SET ON PARAMETER log_statement_stats TO swr_kept",
            "CREATE DATABASE swr_d2",
        ],
    );
    conn.close();
    let mut conn = connect(&server, "swr_d2", "postgres");
    run(&mut conn, &["CREATE ROLE swr_made_in_d2 CREATEDB", "DROP ROLE swr_dropped", "GRANT swr_group TO swr_made_in_d2"]);
    conn.close();
    server.restart().unwrap();
    let fingerprint = "SELECT (SELECT string_agg(concat_ws(' ', oid, rolname, rolsuper, rolinherit, rolcreaterole, \
                       rolcreatedb, rolcanlogin, rolreplication, rolbypassrls, rolconnlimit, rolpassword, rolvaliduntil), \
                       '; ' ORDER BY oid) FROM pg_authid), \
                       (SELECT string_agg(concat_ws(' ', oid, roleid, member, grantor, admin_option, inherit_option, \
                       set_option), '; ' ORDER BY oid) FROM pg_auth_members), \
                       (SELECT string_agg(concat_ws(' ', setrole, setconfig), '; ' ORDER BY setrole) \
                       FROM pg_db_role_setting WHERE setdatabase = 0), \
                       (SELECT string_agg(concat_ws(' ', oid, parname, paracl), '; ' ORDER BY oid) FROM pg_parameter_acl)";
    let mut fingerprints = Vec::new();
    for database in ["postgres", "template1", "swr_d2"] {
        let mut conn = connect(&server, database, "postgres");
        fingerprints.push(rows(&mut conn, fingerprint));
        conn.close();
    }
    assert_eq!(fingerprints[1], fingerprints[0]);
    assert_eq!(fingerprints[2], fingerprints[0]);
    let mut conn = connect(&server, "template1", "swr_kept");
    let member = "SELECT current_user, pg_has_role('swr_group', 'MEMBER'), current_setting('work_mem')";
    assert_eq!(rows(&mut conn, member), expected(&[&["swr_kept", "t", "7MB"]]));
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    run(
        &mut conn,
        &[
            "DROP DATABASE swr_d2",
            "REVOKE SET ON PARAMETER log_statement_stats FROM swr_kept",
            "DROP ROLE swr_kept",
            "DROP ROLE swr_made_in_d2",
            "DROP ROLE swr_group",
        ],
    );
    conn.close();
}

#[test]
fn test_roles_stay_in_their_database_with_server_wide_roles_off() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if matches!(target, Target::Postgres { .. }) {
        return;
    }
    let server = Server::start(&target, "behavior:\n  server_wide_roles: false\n").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE swo_user LOGIN PASSWORD 'password'",
            "ALTER ROLE swo_user SET work_mem = '6MB'",
            "CREATE DATABASE swo_d1",
        ],
    );
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    let named = "SELECT count(*) FROM pg_roles WHERE rolname = 'swo_user'";
    assert_eq!(rows(&mut conn, named), expected(&[&["0"]]));
    conn.close();
    let mut conn = connect(&server, "postgres", "swo_user");
    let setting = "SELECT current_user, current_setting('work_mem')";
    assert_eq!(rows(&mut conn, setting), expected(&[&["swo_user", "6MB"]]));
    conn.close();
    let mut conn = connect(&server, "swo_d1", "swo_user");
    assert_eq!(rows(&mut conn, setting), expected(&[&["swo_user", "6MB"]]));
    conn.close();
    let mut conn = connect(&server, "swo_d1", "postgres");
    run(&mut conn, &["ALTER ROLE swo_user CREATEDB"]);
    conn.close();
    let mut conn = connect(&server, "postgres", "postgres");
    let createdb = "SELECT rolcreatedb FROM pg_roles WHERE rolname = 'swo_user'";
    assert_eq!(rows(&mut conn, createdb), expected(&[&["f"]]));
    run(&mut conn, &["DROP DATABASE swo_d1", "DROP ROLE swo_user"]);
    conn.close();
}

#[test]
fn test_user_mappings_stay_in_their_database_with_server_wide_roles() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE swm_user",
            "CREATE FOREIGN DATA WRAPPER swm_fdw",
            "CREATE SERVER swm_server FOREIGN DATA WRAPPER swm_fdw",
            "CREATE USER MAPPING FOR swm_user SERVER swm_server OPTIONS (password 'secret')",
            "CREATE ROLE swm_after",
        ],
    );
    conn.close();
    server.restart().unwrap();
    let mappings = "SELECT srvname, usename, umoptions FROM pg_user_mappings";
    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows(&mut conn, mappings), expected(&[&["swm_server", "swm_user", "{password=secret}"]]));
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    assert_eq!(rows(&mut conn, "SELECT count(*) FROM pg_user_mapping"), expected(&[&["0"]]));
    assert_eq!(rows(&mut conn, "SELECT count(*) FROM pg_roles WHERE rolname = 'swm_after'"), expected(&[&["1"]]));
    conn.close();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &["DROP SERVER swm_server CASCADE", "DROP FOREIGN DATA WRAPPER swm_fdw", "DROP ROLE swm_user", "DROP ROLE swm_after"],
    );
    conn.close();
}

#[test]
fn test_create_database_copies_the_roles_of_its_database_with_server_wide_roles_off() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if matches!(target, Target::Postgres { .. }) {
        return;
    }
    let server = Server::start(&target, "behavior:
  server_wide_roles: false
").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE crt_owner NOLOGIN",
            "CREATE ROLE crt_user LOGIN PASSWORD 'password'",
            "GRANT crt_owner TO crt_user",
            "ALTER ROLE crt_user SET work_mem = '7MB'",
            "ALTER ROLE crt_user IN DATABASE postgres SET maintenance_work_mem = '70MB'",
            "GRANT SET ON PARAMETER log_min_duration_statement TO crt_user",
            "CREATE DATABASE crt_d OWNER crt_owner",
        ],
    );
    conn.close();
    let mut conn = connect(&server, "crt_d", "crt_user");
    let session = "SELECT current_setting('work_mem'), current_setting('maintenance_work_mem')";
    assert_eq!(rows(&mut conn, session), expected(&[&["7MB", "64MB"]]));
    let roles = "SELECT pg_has_role('crt_user', 'crt_owner', 'MEMBER'),                  has_parameter_privilege('crt_user', 'log_min_duration_statement', 'SET'),                  (SELECT datdba::regrole::text FROM pg_database WHERE datname = 'crt_d')";
    assert_eq!(rows(&mut conn, roles), expected(&[&["t", "t", "crt_owner"]]));
    conn.close();
    let mut conn = connect(&server, "template1", "postgres");
    let named = "SELECT count(*) FROM pg_roles WHERE rolname IN ('crt_owner', 'crt_user')";
    assert_eq!(rows(&mut conn, named), expected(&[&["0"]]));
    conn.close();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["DROP DATABASE crt_d", "REVOKE SET ON PARAMETER log_min_duration_statement FROM crt_user"]);
    run(&mut conn, &["DROP ROLE crt_user", "DROP ROLE crt_owner"]);
    conn.close();
}

#[test]
fn test_initial_privileges_of_databases_and_settings_keep_their_roles_in_every_database() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["CREATE ROLE ip_db_role", "CREATE DATABASE ipd_d"]);
    conn.close();
    let mut conn = connect(&server, "ipd_d", "postgres");
    let path = format!("SET extension_control_path = '{}'", testdata_dir().join("extensions").display());
    run(
        &mut conn,
        &[&path, "CREATE EXTENSION dg_initprivs_db", "DROP EXTENSION dg_initprivs_db", "REVOKE CREATE ON DATABASE ipd_d FROM ip_db_role"],
    );
    conn.close();
    let mut conn = connect(&server, "postgres", "postgres");
    let refused = (
        "2BP01".to_string(),
        "role \"ip_db_role\" cannot be dropped because some objects depend on it".to_string(),
        "initial privileges for database ipd_d
privileges for parameter log_lock_waits
initial privileges for parameter log_lock_waits"
            .to_string(),
    );
    assert_eq!(error(&mut conn, "DROP ROLE ip_db_role"), refused);
    run(&mut conn, &["DROP DATABASE ipd_d"]);
    conn.close();
}

#[test]
fn test_drop_owned_and_revoke_leave_no_dependency_on_initial_privileges_of_settings() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    if matches!(target, Target::Postgres { .. }) {
        return;
    }
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let path = format!("SET extension_control_path = '{}'", testdata_dir().join("extensions").display());
    run(&mut conn, &[&path, "CREATE ROLE ip_db_role", "CREATE EXTENSION dg_initprivs_db", "DROP OWNED BY ip_db_role"]);
    let init_privs = "SELECT classoid::regclass::text, initprivs FROM pg_init_privs \
                      WHERE classoid IN ('pg_database'::regclass, 'pg_parameter_acl'::regclass) ORDER BY 1";
    let owners_only =
        expected(&[&["pg_database", "{=Tc/postgres,postgres=CTc/postgres}"], &["pg_parameter_acl", "{postgres=sA/postgres}"]]);
    assert_eq!(rows(&mut conn, init_privs), owners_only);
    run(
        &mut conn,
        &[
            "DROP ROLE ip_db_role",
            "DROP EXTENSION dg_initprivs_db",
            "CREATE ROLE ip_db_role",
            "CREATE EXTENSION dg_initprivs_db",
            "REVOKE SET ON PARAMETER log_lock_waits FROM ip_db_role",
            "REVOKE CREATE ON DATABASE postgres FROM ip_db_role",
        ],
    );
    let refused = (
        "2BP01".to_string(),
        "role \"ip_db_role\" cannot be dropped because some objects depend on it".to_string(),
        "initial privileges for database postgres".to_string(),
    );
    assert_eq!(error(&mut conn, "DROP ROLE ip_db_role"), refused);
    run(&mut conn, &["DROP OWNED BY ip_db_role", "DROP ROLE ip_db_role"]);
    conn.close();
}
