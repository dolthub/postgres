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

//! Tests that a database's connection limit counts each session once and that a renamed database is found only by
//! its new name, after a crash too, as Postgres' `InitPostgres` and `RenameDatabase` do.

use harness::conn::{connect, error, refused, run, value};
use harness::server::{Server, Target};

#[test]
fn test_a_connection_limit_counts_each_session_once() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut admin = connect(&server, "postgres", "postgres");
    run(
        &mut admin,
        &[
            "CREATE DATABASE cl_db",
            "ALTER DATABASE cl_db CONNECTION LIMIT 1",
            "CREATE ROLE cl_user LOGIN PASSWORD 'password'",
        ],
    );
    let first = connect(&server, "cl_db", "cl_user");
    let too_many = ("53300".to_string(), "too many connections for database \"cl_db\"".to_string());
    assert_eq!(refused(&server, "cl_db", "cl_user"), too_many);
    assert_eq!(refused(&server, "cl_db", "cl_user"), too_many);
    let mut superuser = connect(&server, "cl_db", "postgres");
    assert_eq!(value(&mut superuser, "SELECT current_database()"), "cl_db");
    drop(superuser);
    drop(first);
    let mut again = connect(&server, "cl_db", "cl_user");
    assert_eq!(value(&mut again, "SELECT current_database()"), "cl_db");
}

#[test]
fn test_a_renamed_database_is_found_only_by_its_new_name_after_a_crash_too() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut server = Server::start(&target, "").unwrap();
    let mut admin = connect(&server, "postgres", "postgres");
    run(&mut admin, &["CREATE DATABASE rn_db"]);
    let other = connect(&server, "rn_db", "postgres");
    let busy = error(&mut admin, "ALTER DATABASE rn_db RENAME TO rn_db2");
    let detail = "There is 1 other session using the database.".to_string();
    assert_eq!(busy, ("55006".to_string(), "database \"rn_db\" is being accessed by other users".to_string(), detail));
    drop(other);
    run(&mut admin, &["ALTER DATABASE rn_db RENAME TO rn_db2"]);
    let gone = ("3D000".to_string(), "database \"rn_db\" does not exist".to_string());
    assert_eq!(refused(&server, "rn_db", "postgres"), gone);
    let mut renamed = connect(&server, "rn_db2", "postgres");
    assert_eq!(value(&mut renamed, "SELECT current_database()"), "rn_db2");
    drop(renamed);
    drop(admin);
    server.restart().unwrap();
    assert_eq!(refused(&server, "rn_db", "postgres"), gone);
    let mut renamed = connect(&server, "rn_db2", "postgres");
    assert_eq!(value(&mut renamed, "SELECT current_database()"), "rn_db2");
}
