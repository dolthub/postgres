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

//! Tests of notifications that one session's commits send to the sessions listening on their channels.

use std::time::{Duration, Instant};

use harness::conn::connect;
use harness::pgx::Conn;
use harness::server::{Server, Target};

/// start starts a server and connects two sessions to its `postgres` database.
fn start() -> (Server, Conn, Conn) {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let (a, b) = (connect(&server, "postgres", "postgres"), connect(&server, "postgres", "postgres"));
    (server, a, b)
}

/// received waits a moment for notifications to arrive at an idle session and returns each one's sender, channel,
/// and payload.
fn received(conn: &mut Conn) -> Vec<(u32, String, String)> {
    let deadline = Instant::now() + Duration::from_millis(500);
    while conn.receive_message(deadline).unwrap().is_some() {}
    conn.take_notifications().into_iter().map(|n| (n.process_id, n.channel, n.payload)).collect()
}

#[test]
fn test_notifications_arrive_at_an_idle_listener_when_the_sender_commits() {
    let (_server, mut a, mut b) = start();
    b.exec("LISTEN c", &[]).unwrap();
    for sql in [
        "BEGIN",
        "NOTIFY c, 'x'",
        "NOTIFY c, 'x'",
        "NOTIFY c, 'y'",
        "SAVEPOINT s",
        "NOTIFY c, 'gone'",
        "ROLLBACK TO SAVEPOINT s",
        "NOTIFY other, 'unheard'",
    ] {
        a.exec(sql, &[]).unwrap();
    }
    assert_eq!(received(&mut b), []);
    a.exec("COMMIT", &[]).unwrap();
    let pid = a.process_id();
    assert_eq!(received(&mut b), [(pid, "c".into(), "x".into()), (pid, "c".into(), "y".into())]);
    a.exec("BEGIN", &[]).unwrap();
    a.exec("NOTIFY c, 'rolled back'", &[]).unwrap();
    a.exec("ROLLBACK", &[]).unwrap();
    assert_eq!(received(&mut b), []);
}

#[test]
fn test_a_listener_in_a_transaction_receives_when_its_transaction_ends() {
    let (_server, mut a, mut b) = start();
    b.exec("LISTEN c", &[]).unwrap();
    b.exec("BEGIN", &[]).unwrap();
    a.exec("NOTIFY c, 'x'", &[]).unwrap();
    assert_eq!(received(&mut b), []);
    b.exec("SELECT 1", &[]).unwrap();
    assert_eq!(b.take_notifications(), []);
    b.exec("COMMIT", &[]).unwrap();
    let payloads: Vec<String> = b.take_notifications().into_iter().map(|n| n.payload).collect();
    assert_eq!(payloads, ["x"]);
}

#[test]
fn test_a_session_receives_its_own_notifications() {
    let (_server, mut a, _b) = start();
    a.exec("LISTEN c", &[]).unwrap();
    a.exec("NOTIFY c, 'self'", &[]).unwrap();
    let notifications = a.take_notifications();
    assert_eq!(notifications.len(), 1);
    assert_eq!((notifications[0].process_id, notifications[0].payload.as_str()), (a.process_id(), "self"));
}

#[test]
fn test_notifications_arrive_in_commit_order() {
    let (server, mut a, mut b) = start();
    let mut c = connect(&server, "postgres", "postgres");
    b.exec("LISTEN c", &[]).unwrap();
    a.exec("BEGIN", &[]).unwrap();
    a.exec("NOTIFY c, 'a'", &[]).unwrap();
    c.exec("BEGIN", &[]).unwrap();
    c.exec("NOTIFY c, 'c'", &[]).unwrap();
    c.exec("COMMIT", &[]).unwrap();
    a.exec("COMMIT", &[]).unwrap();
    let payloads: Vec<String> = received(&mut b).into_iter().map(|(_, _, payload)| payload).collect();
    assert_eq!(payloads, ["c", "a"]);
}

#[test]
fn test_listen_receives_only_notifications_committed_after_it() {
    let (_server, mut a, mut b) = start();
    a.exec("NOTIFY c, 'before'", &[]).unwrap();
    a.exec("BEGIN", &[]).unwrap();
    a.exec("NOTIFY c, 'during'", &[]).unwrap();
    b.exec("BEGIN", &[]).unwrap();
    b.exec("LISTEN c", &[]).unwrap();
    a.exec("NOTIFY c, 'still during'", &[]).unwrap();
    b.exec("COMMIT", &[]).unwrap();
    a.exec("COMMIT", &[]).unwrap();
    let payloads: Vec<String> = received(&mut b).into_iter().map(|(_, _, payload)| payload).collect();
    assert_eq!(payloads, ["during", "still during"]);
    b.exec("UNLISTEN c", &[]).unwrap();
    a.exec("NOTIFY c, 'after'", &[]).unwrap();
    assert_eq!(received(&mut b), []);
}

#[test]
fn test_a_closed_listener_leaves_the_sender_unaffected() {
    let (_server, mut a, mut b) = start();
    b.exec("LISTEN c", &[]).unwrap();
    b.close();
    std::thread::sleep(Duration::from_millis(200));
    a.exec("NOTIFY c, 'x'", &[]).unwrap();
    let usage = a.simple_query_rows("SELECT pg_notification_queue_usage()").unwrap();
    assert_eq!(usage.rows[0][0].as_deref(), Some(&b"0"[..]));
}

#[test]
#[ignore = "a session cannot connect to another branch of a database yet"]
fn test_a_listener_on_another_branch_receives_nothing() {
    let (server, mut a, _b) = start();
    a.exec("SELECT dolt_branch('other')", &[]).unwrap();
    let mut other = connect(&server, "postgres/other", "postgres");
    other.exec("LISTEN c", &[]).unwrap();
    a.exec("NOTIFY c, 'main'", &[]).unwrap();
    assert_eq!(received(&mut other), []);
}
