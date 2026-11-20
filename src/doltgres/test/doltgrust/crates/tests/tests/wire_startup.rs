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

use harness::pgx::{Conn, ConnConfig};
use harness::server::{Server, Target};

#[test]
fn test_invalid_startup_timezone_returns_error() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let url = format!("postgres://postgres:password@127.0.0.1:{}/postgres", server.port);
    Conn::connect(ConnConfig::parse(&url).unwrap()).unwrap().close();

    let mut config = ConnConfig::parse(&url).unwrap();
    config.runtime_params.push(("timezone".to_string(), "Not/A/Timezone".to_string()));
    let err = Conn::connect(config).err().expect("expected the invalid timezone startup parameter to be rejected");
    let pg = err.pg_error().unwrap_or_else(|| {
        panic!("expected an explicit PostgreSQL error, not a bare connection failure like an unexpected EOF: {err}")
    });
    assert_eq!(pg.fields.code, "22023");
}

#[test]
fn test_a_database_whose_recorded_collation_version_differs_warns_at_connect() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let url = |dbname: &str| format!("postgres://postgres:password@127.0.0.1:{}/{dbname}", server.port);
    let mut conn = Conn::connect(ConnConfig::parse(&url("postgres")).unwrap()).unwrap();
    assert!(conn.take_notices().is_empty());
    conn.simple_query(
        "CREATE DATABASE collmm TEMPLATE template0 LOCALE_PROVIDER builtin BUILTIN_LOCALE 'C.UTF-8' COLLATION_VERSION '0.9'",
    )
    .unwrap();
    let mut mismatched = Conn::connect(ConnConfig::parse(&url("collmm")).unwrap()).unwrap();
    let notices = mismatched.take_notices();
    assert_eq!(notices.len(), 1, "{notices:?}");
    assert_eq!(notices[0].severity, "WARNING");
    assert_eq!(notices[0].code, "01000");
    assert_eq!(notices[0].message, r#"database "collmm" has a collation version mismatch"#);
    assert_eq!(
        notices[0].detail,
        "The database was created using collation version 0.9, but the operating system provides version 1."
    );
    assert_eq!(
        notices[0].hint,
        "Rebuild all objects in this database that use the default collation and run ALTER DATABASE collmm REFRESH \
         COLLATION VERSION, or build PostgreSQL with the right library version."
    );
    mismatched.close();
    conn.simple_query("ALTER DATABASE collmm REFRESH COLLATION VERSION").unwrap();
    let mut refreshed = Conn::connect(ConnConfig::parse(&url("collmm")).unwrap()).unwrap();
    assert!(refreshed.take_notices().is_empty());
    refreshed.close();
    conn.close();
}
