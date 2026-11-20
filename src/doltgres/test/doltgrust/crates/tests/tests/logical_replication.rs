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

//! Subscriptions to a real Postgres publisher: the statements that connect to it, and what they leave in
//! `pg_subscription_rel` and among the publisher's replication slots.

use harness::conn::{connect, error, run, rows};
use harness::publisher::Publisher;
use harness::server::{Server, Target};

#[test]
fn test_subscription_statements_that_connect_to_a_postgres_publisher() {
    let publisher = Publisher::start().unwrap_or_else(|err| panic!("{err}"));
    publisher.query("CREATE TABLE pubt (a int PRIMARY KEY)").unwrap();
    publisher.query("CREATE TABLE pubt2 (b int PRIMARY KEY)").unwrap();
    publisher.query("CREATE PUBLICATION pub FOR TABLE pubt").unwrap();
    publisher.query("CREATE PUBLICATION pub2 FOR TABLE pubt2").unwrap();
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    run(&mut conn, &["CREATE TABLE pubt (a int PRIMARY KEY)", "CREATE TABLE pubt2 (b int PRIMARY KEY)"]);
    let wrong = publisher.conninfo().replace("password=secret", "password=wrong");
    let (code, message, _) = error(&mut conn, &format!("CREATE SUBSCRIPTION s CONNECTION '{wrong}' PUBLICATION pub"));
    let expected = format!(
        "subscription \"s\" could not connect to the publisher: connection to server at \"127.0.0.1\", port {} failed: FATAL:  password authentication failed for user \"postgres\"",
        publisher.port
    );
    assert_eq!((code.as_str(), message.as_str()), ("08006", expected.as_str()));
    let conninfo = publisher.conninfo();
    run(&mut conn, &[&format!("CREATE SUBSCRIPTION s CONNECTION '{conninfo}' PUBLICATION pub WITH (copy_data = false)")]);
    let rels = "SELECT srrelid::regclass::text, srsubstate::text FROM pg_subscription_rel ORDER BY 1";
    assert_eq!(rows(&mut conn, rels), [["pubt", "r"]]);
    assert_eq!(publisher.query("SELECT slot_name, plugin FROM pg_replication_slots").unwrap().trim(), "s|pgoutput");
    run(&mut conn, &["ALTER SUBSCRIPTION s ADD PUBLICATION pub2 WITH (copy_data = false)"]);
    assert_eq!(rows(&mut conn, rels), [["pubt", "r"], ["pubt2", "r"]]);
    run(&mut conn, &["ALTER SUBSCRIPTION s DROP PUBLICATION pub WITH (copy_data = false)"]);
    assert_eq!(rows(&mut conn, rels), [["pubt2", "r"]]);
    let mut other = connect(&server, "postgres", "postgres");
    assert_eq!(rows(&mut other, rels), [["pubt2", "r"]]);
    publisher.query("ALTER PUBLICATION pub2 ADD TABLE pubt").unwrap();
    run(&mut conn, &["ALTER SUBSCRIPTION s REFRESH PUBLICATION WITH (copy_data = false)"]);
    assert_eq!(rows(&mut conn, rels), [["pubt", "r"], ["pubt2", "r"]]);
    assert_eq!(rows(&mut other, rels), [["pubt", "r"], ["pubt2", "r"]]);
    publisher.query("ALTER PUBLICATION pub2 DROP TABLE pubt").unwrap();
    run(&mut conn, &["ALTER SUBSCRIPTION s REFRESH PUBLICATION"]);
    assert_eq!(rows(&mut conn, rels), [["pubt2", "r"]]);
    assert_eq!(rows(&mut other, rels), [["pubt2", "r"]]);
    run(&mut conn, &["ALTER SUBSCRIPTION s DISABLE", "ALTER SUBSCRIPTION s SET (failover = true)"]);
    assert_eq!(publisher.query("SELECT failover FROM pg_replication_slots").unwrap().trim(), "t");
    run(&mut conn, &["DROP SUBSCRIPTION s"]);
    assert_eq!(rows(&mut conn, rels), Vec::<Vec<String>>::new());
    assert_eq!(publisher.query("SELECT count(*) FROM pg_replication_slots").unwrap().trim(), "0");
}

#[test]
fn test_subscription_to_a_host_that_does_not_resolve() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut conn = connect(&server, "postgres", "postgres");
    let sql = "CREATE SUBSCRIPTION s CONNECTION 'host=nonexistent.invalid dbname=postgres' PUBLICATION pub";
    let (code, message, _) = error(&mut conn, sql);
    let expected = "subscription \"s\" could not connect to the publisher: could not translate host name \"nonexistent.invalid\" to address: Name or service not known";
    assert_eq!((code.as_str(), message.as_str()), ("08006", expected));
}
