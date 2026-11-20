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

//! Tests that databases' owners, privileges, and settings follow Postgres across databases, whose roles each
//! database's own tree keeps.

use harness::conn::{connect, error, expected, rows, run};
use harness::pgx::{Conn, ConnConfig};
use harness::server::{Server, Target};

#[test]
fn test_database_owners_across_database_trees() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE cd_other LOGIN PASSWORD 'password'",
            "CREATE ROLE cd_creator CREATEDB LOGIN PASSWORD 'password'",
            "CREATE ROLE cd_owner LOGIN PASSWORD 'password'",
            "GRANT cd_owner TO cd_creator",
            "SET ROLE cd_creator",
            "CREATE DATABASE cd_d1",
            "CREATE DATABASE cd_d2 OWNER cd_owner",
        ],
    );
    let set_role = |role: &str| ("42501".to_string(), format!("must be able to SET ROLE \"{role}\""), String::new());
    assert_eq!(error(&mut conn, "CREATE DATABASE cd_d3 OWNER cd_other"), set_role("cd_other"));
    let missing = ("42704".to_string(), "role \"cd_missing\" does not exist".to_string(), String::new());
    assert_eq!(error(&mut conn, "CREATE DATABASE cd_d4 OWNER cd_missing"), missing);
    run(&mut conn, &["RESET ROLE"]);
    let owners =
        "SELECT datname, pg_get_userbyid(datdba) AS owner FROM pg_database WHERE datname LIKE 'cd\\_%' ORDER BY 1";
    assert_eq!(rows(&mut conn, owners), expected(&[&["cd_d1", "cd_creator"], &["cd_d2", "cd_owner"]]));
    run(&mut conn, &["SET ROLE cd_other"]);
    let denied = ("42501".to_string(), "permission denied to create database".to_string(), String::new());
    assert_eq!(error(&mut conn, "CREATE DATABASE cd_d5 TEMPLATE cd_d2"), denied);
    run(&mut conn, &["RESET ROLE", "ALTER ROLE cd_other CREATEDB", "SET ROLE cd_other"]);
    let copy = ("42501".to_string(), "permission denied to copy database \"cd_d2\"".to_string(), String::new());
    assert_eq!(error(&mut conn, "CREATE DATABASE cd_d5 TEMPLATE cd_d2"), copy);
    run(&mut conn, &["RESET ROLE", "SET ROLE cd_creator", "CREATE DATABASE cd_d6 TEMPLATE cd_d2"]);
    run(&mut conn, &["ALTER DATABASE cd_d1 OWNER TO cd_owner"]);
    assert_eq!(error(&mut conn, "ALTER DATABASE cd_d2 OWNER TO cd_other"), set_role("cd_other"));
    let not_owner = ("42501".to_string(), "must be owner of database postgres".to_string(), String::new());
    assert_eq!(error(&mut conn, "ALTER DATABASE postgres OWNER TO cd_owner"), not_owner);
    let no_database = ("3D000".to_string(), "database \"cd_missing\" does not exist".to_string(), String::new());
    assert_eq!(error(&mut conn, "ALTER DATABASE cd_missing OWNER TO cd_owner"), no_database);
    run(&mut conn, &["RESET ROLE", "ALTER DATABASE cd_d2 OWNER TO cd_other", "ALTER DATABASE cd_d2 OWNER TO cd_other"]);
    let after = expected(&[&["cd_d1", "cd_owner"], &["cd_d2", "cd_other"], &["cd_d6", "cd_creator"]]);
    assert_eq!(rows(&mut conn, owners), after);
    conn.close();

    let mut conn = connect(&server, "cd_d2", "cd_other");
    let who = "SELECT current_user, current_database()";
    assert_eq!(rows(&mut conn, who), expected(&[&["cd_other", "cd_d2"]]));
    let some = "SELECT datname, pg_get_userbyid(datdba) AS owner FROM pg_database WHERE datname IN ('cd_d1', 'cd_d2') ORDER BY 1";
    assert_eq!(rows(&mut conn, some), expected(&[&["cd_d1", "cd_owner"], &["cd_d2", "cd_other"]]));
    conn.close();

    let mut conn = connect(&server, "cd_d1", "cd_creator");
    assert_eq!(rows(&mut conn, who), expected(&[&["cd_creator", "cd_d1"]]));
    conn.close();

    let mut conn = connect(&server, "cd_d1", "postgres");
    run(&mut conn, &["CREATE ROLE cd_new"]);
    let distinct = "SELECT count(*) = count(DISTINCT oid) AS distinct_oids FROM pg_authid";
    assert_eq!(rows(&mut conn, distinct), expected(&[&["t"]]));
    conn.close();

    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows(&mut conn, owners), after);
    conn.close();
}

#[test]
fn test_database_privileges_and_settings_across_database_trees() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "template1", "postgres");
    run(&mut conn, &["ALTER ROLE ALL SET work_mem = '3MB'", "ALTER DATABASE template1 SET statement_timeout = '4s'"]);
    conn.close();

    let mut conn = connect(&server, "postgres", "postgres");
    run(
        &mut conn,
        &[
            "CREATE ROLE dp_user LOGIN PASSWORD 'password'",
            "CREATE DATABASE dp_d1",
            "GRANT CONNECT, TEMPORARY ON DATABASE dp_d1 TO dp_user",
            "REVOKE CONNECT ON DATABASE dp_d1 FROM PUBLIC",
            "ALTER ROLE dp_user IN DATABASE dp_d1 SET work_mem = '5MB'",
            "ALTER DATABASE dp_d1 SET statement_timeout = '3s'",
            "ALTER ROLE dp_user SET maintenance_work_mem = '70MB'",
        ],
    );
    let acl = "SELECT datname, datacl FROM pg_database WHERE datname = 'dp_d1'";
    let granted = expected(&[&["dp_d1", "{=T/postgres,postgres=CTc/postgres,dp_user=Tc/postgres}"]]);
    assert_eq!(rows(&mut conn, acl), granted);
    let settings = "SELECT coalesce(d.datname, '-'), coalesce(r.rolname, '-'), s.setconfig::text FROM pg_db_role_setting s                     LEFT JOIN pg_database d ON d.oid = s.setdatabase LEFT JOIN pg_roles r ON r.oid = s.setrole                     WHERE d.datname = 'dp_d1' OR r.rolname = 'dp_user' ORDER BY 1, 2";
    let stored = expected(&[
        &["-", "dp_user", "{maintenance_work_mem=70MB}"],
        &["dp_d1", "-", "{statement_timeout=3s}"],
        &["dp_d1", "dp_user", "{work_mem=5MB}"],
    ]);
    assert_eq!(rows(&mut conn, settings), stored);
    let depends = ("2BP01".to_string(), "role \"dp_user\" cannot be dropped because some objects depend on it".to_string());
    let (code, message, _) = error(&mut conn, "DROP ROLE dp_user");
    assert_eq!((code, message), depends);
    conn.close();

    let mut conn = connect(&server, "dp_d1", "dp_user");
    let shown = "SELECT current_setting('work_mem'), current_setting('statement_timeout')";
    assert_eq!(rows(&mut conn, shown), expected(&[&["5MB", "3s"]]));
    let every_database = "SELECT current_setting('maintenance_work_mem')";
    assert_eq!(rows(&mut conn, every_database), expected(&[&["70MB"]]));
    assert_eq!(rows(&mut conn, acl), granted);
    conn.close();

    let mut conn = connect(&server, "dp_d1", "postgres");
    let copied = "SELECT current_setting('work_mem'), current_setting('statement_timeout')";
    assert_eq!(rows(&mut conn, copied), expected(&[&["3MB", "3s"]]));
    run(&mut conn, &["REVOKE ALL ON DATABASE dp_d1 FROM dp_user"]);
    conn.close();

    let url = format!("postgres://dp_user:password@127.0.0.1:{}/dp_d1?sslmode=disable", server.port);
    let Err(refused) = Conn::connect(ConnConfig::parse(&url).unwrap()) else { panic!("dp_user connected without CONNECT") };
    let fields = &refused.pg_error().unwrap_or_else(|| panic!("{refused}")).fields;
    assert_eq!((fields.code.as_str(), fields.message.as_str()), ("42501", "permission denied for database \"dp_d1\""));

    let mut conn = connect(&server, "postgres", "postgres");
    assert_eq!(rows(&mut conn, acl), expected(&[&["dp_d1", "{=T/postgres,postgres=CTc/postgres}"]]));
    run(
        &mut conn,
        &[
            "ALTER DATABASE dp_d1 RESET ALL",
            "ALTER ROLE dp_user IN DATABASE dp_d1 RESET ALL",
            "ALTER ROLE dp_user RESET ALL",
            "DROP DATABASE dp_d1",
            "DROP ROLE dp_user",
        ],
    );
    assert_eq!(rows(&mut conn, settings), expected(&[]));
    conn.close();
}
