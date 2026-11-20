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

//! Script tests: setup statements followed by assertions, each run against a fresh server. Every assertion is sent
//! with the same protocol messages that the Go suite's pgx client sends, and its outcome is compared exactly against
//! what Postgres returns.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::PathBuf;
use std::sync::mpsc;
use std::time::Duration;

use pgproto::ErrorFields;

use crate::decode::decode_binary;
use crate::order::{self, Order, Statements};
use crate::pgx::{self, Arg, Conn, ConnConfig, QueryExecMode, Recorder, Time, formats};
use crate::plan::PlanFact;
use crate::prepared;
use crate::server::{Server, Target};

/// How long a blocking assertion must stay unfinished.
const EXPECTED_BLOCKING_TIMEOUT: Duration = Duration::from_millis(200);
/// How long a blocked assertion may take to finish once it is unblocked.
const UNBLOCK_TIMEOUT: Duration = Duration::from_secs(5);
/// How long settling a blocked assertion waits for it to finish between its checks for a lock it waits on.
const SETTLE_POLL: Duration = Duration::from_millis(10);

/// ScriptTest is a set of setup statements and assertions that run against their own fresh server.
#[derive(Clone, Copy, Debug)]
pub struct ScriptTest {
    /// The name of the test.
    pub name: &'static str,
    /// What a test ported from Dolt checks, in one sentence its reviewer wrote, which its failures print.
    pub intent: &'static str,
    /// The database to create and use, where empty means "postgres".
    pub database: &'static str,
    /// Statements that run before the assertions, which must succeed and whose results are not checked.
    pub set_up_script: &'static [&'static str],
    /// The assertions, in order.
    pub assertions: &'static [ScriptTestAssertion],
    /// When any test in a run sets this, only those tests run. It must never be committed.
    pub focus: bool,
    /// Skips the whole test, including its setup, for the given reason.
    pub skip: Option<&'static str>,
    /// YAML appended to the doltgrust config file.
    pub server_config: &'static str,
}

/// S is a ScriptTest with every field at its default, for use with struct update syntax.
pub const S: ScriptTest = ScriptTest {
    name: "",
    intent: "",
    database: "",
    set_up_script: &[],
    assertions: &[],
    focus: false,
    skip: None,
    server_config: "",
};

/// ScriptTestAssertion is a single statement and its expected outcome.
#[derive(Clone, Copy, Debug)]
pub struct ScriptTestAssertion {
    /// The statement to run.
    pub query: &'static str,
    /// The parameter values, which are encoded the way pgx encodes the corresponding Go values.
    pub bind_vars: &'static [BindVar],
    /// The expected outcome.
    pub expected: Expected,
    /// The expected notices, in order.
    pub notices: &'static [Diagnostic],
    /// How the statement is sent, which defaults to how the Go suite sends an assertion with this outcome.
    pub flow: Flow,
    /// Runs the statement as this user on its own connection, where empty means the default connection.
    pub username: &'static str,
    /// The password of the user.
    pub password: &'static str,
    /// The named persistent client that runs the statement, which connects as the assertion's user when one is given,
    /// and otherwise like the default connection.
    pub client: &'static str,
    /// The database the named client connects to, where empty means the script's.
    pub database: &'static str,
    /// Expects the statement to block until a later assertion on another client unblocks it.
    pub expected_blocking: bool,
    /// Closes the named client instead of running the statement.
    pub close_client: bool,
    /// Sends this testdata file to a COPY FROM STDIN statement.
    pub copy_from_stdin_file: &'static str,
    /// Expects a COPY TO STDOUT statement to send the contents of this testdata file.
    pub copy_to_stdout_file: &'static str,
    /// Pipes the output of a COPY TO STDOUT statement into this COPY FROM STDIN statement.
    pub copy_round_trip_stdin_query: &'static str,
    /// Prepares the statement under this name, as pgx's Prepare does, instead of running it. A later assertion runs
    /// it by using the name as its query.
    pub prepare: &'static str,
    /// When any assertion in a test sets this, only those assertions run. It must never be committed.
    pub focus: bool,
    /// Skips the assertion for the given reason.
    pub skip: Option<&'static str>,
    /// How an assertion ported from Dolt departs from Dolt's own expectation, which its failures print.
    pub dolt: Option<Dolt>,
    /// Why an EXPLAIN's plan differs from Postgres' by design, with the lines Doltgrust prints instead, which a Doltgrust
    /// target checks in place of the expectation.
    pub diverged: Option<(&'static str, &'static [&'static str])>,
}

/// PRIMARY_KEY_RETURNED is why a plan differs from Postgres' when an index-only scan returns a primary key column.
pub const PRIMARY_KEY_RETURNED: &str = "Doltgrust's secondary index entries end with the primary key, so an Index Only Scan returns it (the owner, 2026-11-01)";

/// Dolt is how an assertion ported from Dolt departs from what Dolt's test expects.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dolt {
    /// Postgres forces Doltgrust to differ from Dolt, for this reason, which names its Appendix C row.
    Differs(&'static str),
    /// Dolt's expectation is a Dolt bug, which this explains, so the assertion expects the right behavior.
    Bug(&'static str),
    /// This MySQL-only part of Dolt's assertion has no Postgres meaning and was left out.
    MySqlOnly(&'static str),
    /// Dolt's expectation shows its test harness's own commits, which this names, so the assertion expects what a
    /// fresh Doltgrust database shows.
    Setup(&'static str),
}

/// A is a ScriptTestAssertion with every field at its default, for use with struct update syntax.
pub const A: ScriptTestAssertion = ScriptTestAssertion {
    query: "",
    bind_vars: &[],
    expected: Expected::Ok,
    notices: &[],
    flow: Flow::Auto,
    username: "",
    password: "",
    client: "",
    database: "",
    expected_blocking: false,
    close_client: false,
    copy_from_stdin_file: "",
    copy_to_stdout_file: "",
    copy_round_trip_stdin_query: "",
    prepare: "",
    focus: false,
    skip: None,
    dolt: None,
    diverged: None,
};

/// Expected is the expected outcome of an assertion.
#[derive(Clone, Copy, Debug)]
pub enum Expected {
    /// The statement succeeds, and nothing else is checked.
    Ok,
    /// The statement returns these columns, rows, and command tag. Rows are compared in any order the query allows,
    /// as `order::Statements` decides.
    Rows {
        /// The result columns.
        columns: &'static [Column],
        /// The rows.
        rows: &'static [&'static [Cell]],
        /// The command tag.
        tag: &'static str,
    },
    /// The statement succeeds with this command tag and returns no result columns, such as most DDL and DML.
    Tag(&'static str),
    /// The statement fails with this error.
    Error(Diagnostic),
    /// The client fails before the server can answer, with an error containing this text, such as a parameter that
    /// pgx cannot encode.
    ClientError(&'static str),
    /// The statement is an EXPLAIN whose plan shows these facts. Plans are implementation details, so only the facts are
    /// checked, and a real Postgres, whose planner chooses differently, skips them.
    Plan(&'static [PlanFact]),
}

/// Flow is how an assertion's statement is sent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flow {
    /// Query when the statement succeeds with rows or a tag, and Exec when it fails or is unchecked, which is how
    /// the Go suite sends most assertions.
    Auto,
    /// pgx's Exec: the simple protocol without parameters, and a prepared statement with them.
    Exec,
    /// pgx's Query: a described statement executed with pgx's result formats.
    Query,
    /// pgx's Query in the simple protocol mode, which the Go enginetests use for every statement.
    Simple,
}

/// Column is an expected result column: its name, and its type OID or USER_DEFINED for any type outside the
/// built-in range.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Column(pub &'static str, pub u32);

/// USER_DEFINED matches any type OID of 16384 or more, since user-defined types get arbitrary OIDs.
pub const USER_DEFINED: u32 = 0;

/// Cell is an expected value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cell {
    /// The SQL NULL.
    Null,
    /// A value with this text. Binary values are rendered by the decoder.
    Text(&'static str),
    /// Any value that is not NULL, for values that are inherently arbitrary such as OIDs and process IDs.
    Any,
    /// A float8 value within two units in the last place of this one, for results that Postgres computes with the
    /// platform's math library, whose last digit differs between platforms.
    Approx(&'static str),
    /// An OID that Postgres gave a user object, which matches the OID the server gave the same object: within a
    /// script, each expected OID matches one OID of the server, and different expected OIDs match different ones.
    Oid(u32),
}

/// Diagnostic is an expected error or notice. Empty strings and zero positions mean the field is absent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    /// The severity, such as ERROR or NOTICE.
    pub severity: &'static str,
    /// The SQLSTATE code.
    pub code: &'static str,
    /// The message.
    pub message: &'static str,
    /// Matches any message that contains the message, as a fallback where an exact match is prohibitive.
    pub message_contains: bool,
    /// The detail.
    pub detail: &'static str,
    /// Matches any detail that contains the detail, for a detail that reports timings.
    pub detail_contains: bool,
    /// The hint.
    pub hint: &'static str,
    /// The one-based character position in the query.
    pub position: i32,
    /// The schema name.
    pub schema: &'static str,
    /// The table name.
    pub table: &'static str,
    /// The column name.
    pub column: &'static str,
    /// The data type name.
    pub data_type: &'static str,
    /// The constraint name.
    pub constraint: &'static str,
    /// The context, its lines joined by newlines, or None to leave it unchecked.
    pub context: Option<&'static str>,
}

/// E is an ERROR Diagnostic with every other field at its default, for use with struct update syntax.
pub const E: Diagnostic = Diagnostic {
    severity: "ERROR",
    code: "",
    message: "",
    message_contains: false,
    detail: "",
    detail_contains: false,
    hint: "",
    position: 0,
    schema: "",
    table: "",
    column: "",
    data_type: "",
    constraint: "",
    context: None,
};

/// N is a NOTICE Diagnostic with every other field at its default, for use with struct update syntax.
pub const N: Diagnostic = Diagnostic { severity: "NOTICE", ..E };

/// BindVar is a parameter value, mirroring the Go value a test would pass to pgx.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BindVar {
    /// An untyped Go nil.
    Null,
    /// A Go int.
    Int(i64),
    /// A Go int32.
    Int32(i32),
    /// A Go int64.
    Int64(i64),
    /// A Go float64.
    Float64(f64),
    /// A Go bool.
    Bool(bool),
    /// A Go string.
    Str(&'static str),
    /// A Go []byte.
    Bytes(&'static [u8]),
    /// A Go time.Time.
    Time(Time),
    /// A pgtype.Date.
    Date(Time),
    /// A pgtype.Timestamp.
    Timestamp(Time),
    /// A pgtype.Numeric scanned from the text.
    Numeric(&'static str),
    /// A pgtype.UUID.
    Uuid([u8; 16]),
    /// A Go []string.
    StrArray(&'static [&'static str]),
    /// A Go []int32.
    Int32Array(&'static [i32]),
}

impl BindVar {
    /// to_arg converts the value to the client's argument type.
    fn to_arg(self) -> Arg {
        match self {
            BindVar::Null => Arg::Null,
            BindVar::Int(v) => Arg::Int(v),
            BindVar::Int32(v) => Arg::Int32(v),
            BindVar::Int64(v) => Arg::Int64(v),
            BindVar::Float64(v) => Arg::Float64(v),
            BindVar::Bool(v) => Arg::Bool(v),
            BindVar::Str(v) => Arg::Str(v.to_string()),
            BindVar::Bytes(v) => Arg::Bytes(v.to_vec()),
            BindVar::Time(v) => Arg::Time(v),
            BindVar::Date(v) => Arg::Date(v),
            BindVar::Timestamp(v) => Arg::Timestamp(v),
            BindVar::Numeric(v) => Arg::Numeric(v.to_string()),
            BindVar::Uuid(v) => Arg::Uuid(v),
            BindVar::StrArray(v) => Arg::StrArray(v.iter().map(|s| s.to_string()).collect()),
            BindVar::Int32Array(v) => Arg::Int32Array(v.to_vec()),
        }
    }
}

/// Observation is everything an assertion returned.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Observation {
    /// The result columns as (name, type OID).
    pub columns: Vec<(String, u32)>,
    /// The rows, rendered as text, where None is NULL.
    pub rows: Vec<Vec<Option<String>>>,
    /// Whether the statement was sent with Query, which is when its rows are known.
    pub queried: bool,
    /// The command tag.
    pub tag: String,
    /// The server's error.
    pub error: Option<ErrorFields>,
    /// An error that did not come from the server, such as a parameter that could not be encoded.
    pub client_error: Option<String>,
    /// The notices received while running the statement.
    pub notices: Vec<ErrorFields>,
    /// The data sent by COPY TO STDOUT.
    pub copy_out: Option<Vec<u8>>,
    /// Whether the assertion was skipped.
    pub skipped: bool,
}

/// testdata_dir returns the directory holding COPY test files: DOLTGRUST_TESTDATA when it is set, and otherwise the
/// testdata directory of the crate whose tests are running, which cargo names in CARGO_MANIFEST_DIR.
pub fn testdata_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("DOLTGRUST_TESTDATA") {
        return PathBuf::from(dir);
    }
    let manifest = std::env::var_os("CARGO_MANIFEST_DIR").map(PathBuf::from).unwrap_or_default();
    manifest.join("testdata")
}

/// BlockedResult is the connection and outcome of a blocking statement once it finishes.
type BlockedResult = (Conn, Result<String, pgx::Error>);

/// Session is the server and connections that a script runs on.
pub struct Session {
    /// The target the server was started from.
    pub target: Target,
    /// The server, which stops when the session is dropped.
    pub server: Server,
    database: String,
    default: Conn,
    other: Option<(Conn, String, String)>,
    clients: HashMap<String, Conn>,
    /// The statements still blocked, by client, with the error each is expected to end with.
    blocked: HashMap<String, (mpsc::Receiver<BlockedResult>, Option<Diagnostic>, u32)>,
    /// The blocked statements that finished before their client's next assertion, by client.
    unblocked: HashMap<String, (BlockedResult, Option<Diagnostic>)>,
    /// The connection that asks whether a blocked statement waits on a lock, made when first needed.
    control: Option<Conn>,
    recorder: Option<Recorder>,
}

/// The environment variable naming the directory that receives recordings of the bytes clients send.
pub const RECORD_DIR_ENV: &str = "DOLTGRUST_RECORD_DIR";

/// The environment variable that, when set, makes a prepared run, as Dolt's prepared runner makes: each assertion of
/// one statement with no parameters of its own goes through the extended protocol, with its constants bound as
/// parameters.
pub const PREPARED_ENV: &str = "DOLTGRUST_TEST_PREPARED";

/// sanitize makes a name safe to use in a file name.
fn sanitize(name: &str) -> String {
    name.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect()
}

impl Session {
    /// start starts a fresh server and connects to the database the way the Go suite does: it creates the database
    /// over a connection without one, then connects to it with describe-exec mode and pings it.
    pub fn start(target: &Target, database: &str, server_config: &str) -> Result<Session, String> {
        let server = Server::start(target, server_config)?;
        let database = if database.is_empty() { "postgres".to_string() } else { database.to_string() };
        let url = |database: &str| {
            format!("postgres://postgres:password@127.0.0.1:{}/{database}?DateStyle=ISO%2C%20MDY", server.port)
        };
        let recorder = std::env::var_os(RECORD_DIR_ENV).map(|_| Recorder::new());
        let mut setup_config = ConnConfig::parse(&url("")).map_err(|err| err.to_string())?;
        setup_config.recorder = recorder.clone();
        let with_log = |err: String| format!("{err}\nserver log:\n{}", server.log_tail());
        let mut setup = connect_with_retries(setup_config).map_err(with_log)?;
        let create = if target.is_postgres() {
            (database != "postgres").then(|| format!("CREATE DATABASE {database}"))
        } else {
            Some(format!("CREATE DATABASE IF NOT EXISTS {database}"))
        };
        if let Some(create) = create {
            setup.exec(&create, &[]).map_err(|err| format!("{create}: {err}"))?;
        }
        setup.close();
        let mut config = ConnConfig::parse(&url(&database)).map_err(|err| err.to_string())?;
        config.default_query_exec_mode = QueryExecMode::DescribeExec;
        config.recorder = recorder.clone();
        let mut default = Conn::connect(config).map_err(|err| with_log(err.to_string()))?;
        default.ping().map_err(|err| format!("ping: {err}"))?;
        let temp_dir = server.directory().join("tmp");
        std::fs::create_dir_all(&temp_dir).map_err(|err| err.to_string())?;
        SCRIPT_TEMP_DIR.with(|dir| *dir.borrow_mut() = Some(temp_dir));
        Ok(Session {
            target: target.clone(),
            server,
            database,
            default,
            other: None,
            clients: HashMap::new(),
            blocked: HashMap::new(),
            unblocked: HashMap::new(),
            control: None,
            recorder,
        })
    }

    /// save_recording writes the bytes every connection sent, one hexadecimal line per connection, to a file in
    /// DOLTGRUST_RECORD_DIR named after the running test and script, when recording is enabled.
    pub fn save_recording(&self, script_name: &str) {
        let (Some(recorder), Some(dir)) = (&self.recorder, std::env::var_os(RECORD_DIR_ENV)) else { return };
        let test =
            std::thread::current().name().unwrap_or("unknown").rsplit("::").next().unwrap_or("unknown").to_string();
        let base = format!("{test}__{}", sanitize(script_name));
        let mut path = PathBuf::from(&dir).join(format!("{base}.hex"));
        let mut counter = 1;
        while path.exists() {
            counter += 1;
            path = PathBuf::from(&dir).join(format!("{base}__{counter}.hex"));
        }
        let text: String = recorder
            .take()
            .iter()
            .map(|bytes| bytes.iter().map(|b| format!("{b:02x}")).collect::<String>() + "\n")
            .collect();
        let _ = std::fs::write(path, text);
    }

    /// set_up runs a setup statement on the default connection, or for psql's `\c <database>` connects the default
    /// connection to that database for the rest of the script.
    pub fn set_up(&mut self, query: &str) -> Result<(), String> {
        if let Some(database) = query.strip_prefix("\\c ") {
            let config = ConnConfig { database: database.to_string(), ..self.default.config().clone() };
            let conn = Conn::connect(config).map_err(|err| format!("{query}: {err}"))?;
            std::mem::replace(&mut self.default, conn).close();
            self.database = database.to_string();
            return Ok(());
        }
        let query = if self.target.is_postgres() { postgres_set_up(query) } else { query.to_string() };
        self.default
            .exec(&expand(&query), &[])
            .map(|_| ())
            .map_err(|err| format!("error running setup query: {query}: {err}"))
    }

    /// connection returns the connection for an assertion's user, connecting as Connection.Connect does in the Go
    /// suite: the default connection for no user, and otherwise one connection per user and password.
    fn connection(&mut self, username: &str, password: &str) -> Result<&mut Conn, pgx::Error> {
        if username.is_empty() {
            if let Some((mut other, _, _)) = self.other.take() {
                other.close();
            }
            return Ok(&mut self.default);
        }
        let reuse = matches!(&self.other, Some((_, user, pass)) if user == username && pass == password);
        if !reuse {
            if let Some((mut other, _, _)) = self.other.take() {
                other.close();
            }
            let url = format!("postgres://{username}:{password}@127.0.0.1:{}/{}", self.server.port, self.database);
            let mut config = ConnConfig::parse(&url)?;
            config.recorder = self.recorder.clone();
            let conn = Conn::connect(config)?;
            self.other = Some((conn, username.to_string(), password.to_string()));
        }
        Ok(&mut self.other.as_mut().unwrap().0)
    }

    /// client returns a named client, connecting it to a database the first time: with the default connection's
    /// configuration, as transaction tests do, or as the given user the way pgx.Connect does.
    fn client(&mut self, name: &str, username: &str, password: &str, database: &str) -> Result<&mut Conn, String> {
        if !self.clients.contains_key(name) {
            let database = if database.is_empty() { self.database.as_str() } else { database };
            let config = if username.is_empty() {
                ConnConfig { database: database.to_string(), ..self.default.config().clone() }
            } else {
                let url = format!("postgres://{username}:{password}@127.0.0.1:{}/{database}", self.server.port);
                let mut config = ConnConfig::parse(&url).map_err(|err| err.to_string())?;
                config.recorder = self.recorder.clone();
                config
            };
            let conn = Conn::connect(config).map_err(|err| err.to_string())?;
            self.clients.insert(name.to_string(), conn);
        }
        Ok(self.clients.get_mut(name).unwrap())
    }

    /// run runs an assertion and returns what it observed. Transaction-test fields are honored when the assertion
    /// names a client.
    pub fn run(&mut self, assertion: &ScriptTestAssertion) -> Observation {
        if assertion.skip.is_some() {
            return Observation { skipped: true, ..Observation::default() };
        }
        self.settle_blocked();
        if !assertion.client.is_empty() {
            return self.run_on_client(assertion);
        }
        let conn = match self.connection(assertion.username, assertion.password) {
            Ok(conn) => conn,
            Err(err) => return error_observation(err, Vec::new()),
        };
        conn.take_notices();
        let mut observation = execute(conn, assertion);
        observation.notices.extend(conn.take_notices());
        observation
    }

    /// run_on_client runs an assertion of a transaction test on its named client.
    fn run_on_client(&mut self, assertion: &ScriptTestAssertion) -> Observation {
        let name = assertion.client.to_string();
        let mut blocked_notices = Vec::new();
        let finished = match self.blocked.remove(&name) {
            Some((receiver, expected, _)) => Some((receiver.recv_timeout(UNBLOCK_TIMEOUT), expected)),
            None => self.unblocked.remove(&name).map(|(result, expected)| (Ok(result), expected)),
        };
        if let Some((finished, expected)) = finished {
            match finished {
                Ok((mut conn, result)) => {
                    blocked_notices = conn.take_notices();
                    self.clients.insert(name.clone(), conn);
                    if let Some(error) = unblocked_error(&name, result, expected) {
                        return Observation { client_error: Some(error), ..Observation::default() };
                    }
                }
                Err(_) => {
                    return Observation {
                        client_error: Some(format!("blocked query for client {name} did not complete")),
                        ..Observation::default()
                    };
                }
            }
        }
        if assertion.close_client {
            return match self.clients.remove(&name) {
                Some(mut conn) => {
                    conn.close();
                    Observation::default()
                }
                None => Observation {
                    client_error: Some(format!("cannot close unknown client {name}")),
                    ..Observation::default()
                },
            };
        }
        if let Err(err) = self.client(&name, assertion.username, assertion.password, assertion.database) {
            return Observation { client_error: Some(err), ..Observation::default() };
        }
        if assertion.expected_blocking {
            let mut conn = self.clients.remove(&name).unwrap();
            let query = assertion.query.to_string();
            let args: Vec<Arg> = assertion.bind_vars.iter().map(|v| v.to_arg()).collect();
            let (sender, receiver) = mpsc::channel();
            let process_id = conn.process_id();
            std::thread::spawn(move || {
                let result = conn.exec(&query, &args);
                let _ = sender.send((conn, result));
            });
            return match receiver.recv_timeout(EXPECTED_BLOCKING_TIMEOUT) {
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    let expected = match assertion.expected {
                        Expected::Error(diagnostic) => Some(diagnostic),
                        _ => None,
                    };
                    self.blocked.insert(name, (receiver, expected, process_id));
                    Observation::default()
                }
                Ok((conn, result)) => {
                    self.clients.insert(name, conn);
                    Observation {
                        client_error: Some(format!("query completed before blocking timeout: {result:?}")),
                        ..Observation::default()
                    }
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    Observation { client_error: Some("blocked query thread panicked".into()), ..Observation::default() }
                }
            };
        }
        let conn = self.clients.get_mut(&name).unwrap();
        conn.take_notices();
        let mut observation = execute(conn, assertion);
        blocked_notices.extend(observation.notices);
        blocked_notices.extend(conn.take_notices());
        observation.notices = blocked_notices;
        observation
    }

    /// finish waits for every query that is still blocked, returning an error for any that fail.
    pub fn finish(&mut self) -> Vec<String> {
        let mut errors = Vec::new();
        for (name, (receiver, expected, _)) in self.blocked.drain() {
            match receiver.recv_timeout(UNBLOCK_TIMEOUT) {
                Ok((_, result)) => errors.extend(unblocked_error(&name, result, expected)),
                Err(_) => errors.push(format!("blocked query for client {name} did not complete")),
            }
        }
        for (name, ((_, result), expected)) in self.unblocked.drain() {
            errors.extend(unblocked_error(&name, result, expected));
        }
        errors
    }

    /// settle_blocked waits, before the next assertion, for each blocked statement to finish or to wait on a lock,
    /// as Postgres' isolationtester waits for a step until it completes or `pg_isolation_test_session_is_blocked`
    /// reports it blocked, so that a step that unblocked another runs before the assertions after it.
    fn settle_blocked(&mut self) {
        let names: Vec<String> = self.blocked.keys().cloned().collect();
        for name in names {
            let deadline = std::time::Instant::now() + UNBLOCK_TIMEOUT;
            while let Some((receiver, _, process_id)) = self.blocked.get(&name) {
                let process_id = *process_id;
                match receiver.recv_timeout(SETTLE_POLL) {
                    Ok(result) => {
                        let expected = self.blocked.remove(&name).and_then(|(_, expected, _)| expected);
                        self.unblocked.insert(name.clone(), (result, expected));
                        break;
                    }
                    Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => {
                        if std::time::Instant::now() >= deadline || self.waits_on_lock(process_id) {
                            break;
                        }
                    }
                }
            }
        }
    }

    /// waits_on_lock reports whether the session of a process id waits on a lock another session holds.
    fn waits_on_lock(&mut self, process_id: u32) -> bool {
        if self.control.is_none() {
            let Ok(conn) = Conn::connect(self.default.config().clone()) else { return false };
            self.control = Some(conn);
        }
        let Some(control) = self.control.as_mut() else { return false };
        let query = format!("SELECT (cardinality(pg_blocking_pids({process_id})) > 0)::text");
        control.query(&query, &[]).is_ok_and(|result| {
            result.rows.first().and_then(|row| row.first()).is_some_and(|value| value.as_deref() == Some(b"true"))
        })
    }
}

/// unblocked_error returns why a blocked statement's result differs from the error it is expected to end with, or
/// from success when none is expected.
fn unblocked_error(name: &str, result: Result<String, pgx::Error>, expected: Option<Diagnostic>) -> Option<String> {
    match (result, expected) {
        (Ok(_), None) => None,
        (Ok(_), Some(expected)) => {
            Some(format!("blocked query for client {name} succeeded where {} was expected", expected.code))
        }
        (Err(err), Some(expected))
            if err.pg_error().is_some_and(|pg| {
                pg.fields.code == expected.code
                    && match expected.message_contains {
                        true => pg.fields.message.contains(expected.message),
                        false => pg.fields.message == expected.message,
                    }
            }) =>
        {
            None
        }
        (Err(err), _) => Some(format!("blocked query for client {name} failed: {err}")),
    }
}

/// connect_with_retries connects, retrying like the Go suite does in case the server is not ready.
fn connect_with_retries(config: ConnConfig) -> Result<Conn, String> {
    let mut last = String::new();
    for attempt in 0..3 {
        match Conn::connect(config.clone()) {
            Ok(conn) => return Ok(conn),
            Err(err) => last = err.to_string(),
        }
        if attempt < 2 {
            std::thread::sleep(Duration::from_secs(1));
        }
    }
    Err(last)
}

/// error_observation turns a client error into an observation.
fn error_observation(err: pgx::Error, notices: Vec<ErrorFields>) -> Observation {
    match err.pg_error() {
        Some(pg) => Observation { error: Some(pg.fields.clone()), notices, ..Observation::default() },
        None => Observation { client_error: Some(err.to_string()), notices, ..Observation::default() },
    }
}

/// effective_flow returns how an assertion is sent, choosing like the Go suite when the flow is automatic.
pub fn effective_flow(assertion: &ScriptTestAssertion) -> Flow {
    match assertion.flow {
        Flow::Auto => match assertion.expected {
            Expected::Rows { .. } | Expected::Tag(_) => Flow::Query,
            Expected::Ok | Expected::Error(_) | Expected::ClientError(_) => Flow::Exec,
            Expected::Plan(_) => Flow::Query,
        },
        flow => flow,
    }
}

/// check_diverged compares the plan a Doltgrust target printed for an assertion whose plan differs from Postgres' by
/// design against the lines it expects instead, returning the problems.
fn check_diverged(lines: &[&str], observation: &Observation) -> Vec<String> {
    let printed: Vec<Option<&str>> =
        observation.rows.iter().map(|row| row.first().and_then(|c| c.as_deref())).collect();
    match observation.error.is_none() && printed.iter().copied().eq(lines.iter().map(|&line| Some(line))) {
        true => Vec::new(),
        false => {
            vec![format!("expected Doltgrust's plan {lines:?}, got {printed:?} with error {:?}", observation.error)]
        }
    }
}

/// run_assertion runs an assertion on the connection, returning its problems.
pub fn run_assertion(conn: &mut Conn, assertion: &ScriptTestAssertion) -> Vec<String> {
    check(assertion, &execute(conn, assertion))
}

/// execute sends an assertion's statement on the connection and observes the outcome.
fn execute(conn: &mut Conn, assertion: &ScriptTestAssertion) -> Observation {
    let query = expand(assertion.query);
    let args: Vec<Arg> = assertion.bind_vars.iter().map(|v| v.to_arg()).collect();
    if !assertion.prepare.is_empty() {
        return match conn.prepare(assertion.prepare, &query) {
            Ok(_) => Observation::default(),
            Err(err) => error_observation(err, Vec::new()),
        };
    }
    if !assertion.copy_from_stdin_file.is_empty() {
        let data = match std::fs::read(testdata_dir().join(assertion.copy_from_stdin_file)) {
            Ok(data) => data,
            Err(err) => return Observation { client_error: Some(err.to_string()), ..Observation::default() },
        };
        return match conn.copy_from(&query, &data) {
            Ok(tag) => Observation { tag, ..Observation::default() },
            Err(err) => error_observation(err, Vec::new()),
        };
    }
    if !assertion.copy_round_trip_stdin_query.is_empty() {
        return match conn.copy_to(&query) {
            Ok((data, _)) => match conn.copy_from(&expand(assertion.copy_round_trip_stdin_query), &data) {
                Ok(tag) => Observation { tag, copy_out: Some(data), ..Observation::default() },
                Err(err) => error_observation(err, Vec::new()),
            },
            Err(err) => error_observation(err, Vec::new()),
        };
    }
    if !assertion.copy_to_stdout_file.is_empty() {
        return match conn.copy_to(&query) {
            Ok((data, tag)) => Observation { tag, copy_out: Some(data), ..Observation::default() },
            Err(err) => error_observation(err, Vec::new()),
        };
    }
    let prepared =
        if args.is_empty() && std::env::var_os(PREPARED_ENV).is_some() { prepared::bind(&query) } else { None };
    let flow = if prepared.is_some() { Flow::Query } else { effective_flow(assertion) };
    match flow {
        Flow::Query | Flow::Simple => {
            let result = match &prepared {
                Some((sql, types, values)) => conn.query_with_param_oids(sql, types, values),
                None if flow == Flow::Simple => conn.simple_query_rows(&query),
                None => conn.query(&query, &args),
            };
            match result {
                Ok(result) => {
                    let mut observation = Observation {
                        columns: result.fields.iter().map(|f| (f.name.clone(), f.data_type_oid)).collect(),
                        queried: true,
                        tag: result.command_tag,
                        ..Observation::default()
                    };
                    for row in &result.rows {
                        let mut cells = Vec::with_capacity(row.len());
                        for (value, field) in row.iter().zip(&result.fields) {
                            cells.push(match value {
                                None => None,
                                Some(bytes) => Some(match render(conn, field.data_type_oid, field.format, bytes) {
                                    Ok(text) => text,
                                    Err(err) => {
                                        observation.client_error = Some(err);
                                        String::new()
                                    }
                                }),
                            });
                        }
                        observation.rows.push(cells);
                    }
                    if let Some(err) = result.error {
                        let error = error_observation(err, Vec::new());
                        observation.error = error.error;
                        observation.client_error = observation.client_error.or(error.client_error);
                    }
                    observation
                }
                Err(err) => error_observation(err, Vec::new()),
            }
        }
        _ => match conn.exec(&query, &args) {
            Ok(tag) => Observation { tag, ..Observation::default() },
            Err(err) => error_observation(err, Vec::new()),
        },
    }
}

/// TESTDATA_TOKEN stands for the absolute path of the testdata directory in queries and expectations, for statements
/// that make the server read a file.
pub const TESTDATA_TOKEN: &str = "{TESTDATA}";

/// TEMPDIR_TOKEN stands for the temporary directory of the running script, which is new for each script.
pub const TEMPDIR_TOKEN: &str = "{TEMPDIR}";

/// NEWDIR_PREFIX starts a `{NEWDIR:name}` token, which stands for a directory of that name in the script's temporary
/// directory, created when the token is first expanded.
pub const NEWDIR_PREFIX: &str = "{NEWDIR:";

thread_local! {
    /// SCRIPT_TEMP_DIR is the temporary directory of the session that this thread last started.
    static SCRIPT_TEMP_DIR: std::cell::RefCell<Option<PathBuf>> = const { std::cell::RefCell::new(None) };
    /// OID_BINDINGS maps each expected OID of the running script to the server's OID that it matched.
    static OID_BINDINGS: std::cell::RefCell<HashMap<u32, String>> = std::cell::RefCell::new(HashMap::new());
    /// STATEMENTS reads the running script's statements, deciding the orders their rows may come in.
    static STATEMENTS: std::cell::RefCell<Statements> = std::cell::RefCell::new(Statements::default());
}

/// postgres_set_up rewrites Doltgrust's `CREATE USER|ROLE IF NOT EXISTS 'name'` into the statement Postgres accepts,
/// so that scripts whose setup creates roles that way can run against Postgres.
fn postgres_set_up(query: &str) -> String {
    let lower = query.to_ascii_lowercase();
    for prefix in ["create user if not exists ", "create role if not exists "] {
        if let Some(rest) = lower.strip_prefix(prefix) {
            let rest = &query[query.len() - rest.len()..];
            let rest = match rest.strip_prefix('\'') {
                Some(quoted) => quoted.replacen('\'', "", 1),
                None => rest.to_string(),
            };
            return format!("{}{rest}", &query[..prefix.len() - "if not exists ".len()]);
        }
    }
    query.to_string()
}

/// expand replaces TESTDATA_TOKEN with the absolute path of the testdata directory, and TEMPDIR_TOKEN and NEWDIR
/// tokens with paths in the running script's temporary directory.
pub fn expand(text: &str) -> std::borrow::Cow<'_, str> {
    if !text.contains(TESTDATA_TOKEN) && !text.contains(TEMPDIR_TOKEN) && !text.contains(NEWDIR_PREFIX) {
        return std::borrow::Cow::Borrowed(text);
    }
    let dir = std::fs::canonicalize(testdata_dir()).unwrap_or_else(|_| testdata_dir());
    let mut out = text.replace(TESTDATA_TOKEN, &portable(&dir));
    if out.contains(TEMPDIR_TOKEN) || out.contains(NEWDIR_PREFIX) {
        let temp_dir =
            SCRIPT_TEMP_DIR.with(|dir| dir.borrow().clone()).expect("a temporary directory outside a script");
        while let Some(start) = out.find(NEWDIR_PREFIX) {
            let Some(length) = out[start..].find('}') else { break };
            let path = temp_dir.join(&out[start + NEWDIR_PREFIX.len()..start + length]);
            let _ = std::fs::create_dir_all(&path);
            out.replace_range(start..start + length + 1, &portable(&path));
        }
        out = out.replace(TEMPDIR_TOKEN, &portable(&temp_dir));
    }
    std::borrow::Cow::Owned(out)
}

/// portable returns a path with forward slashes and without the `\\?\` prefix that canonicalizing adds on Windows,
/// so that expanded paths open there and match the file URLs that Dolt prints.
fn portable(path: &std::path::Path) -> String {
    let text = path.to_string_lossy();
    text.strip_prefix(r"\\?\").unwrap_or(&text).replace('\\', "/")
}

/// render renders a result value as text, a text value converted from the connection's client encoding.
fn render(conn: &Conn, oid: u32, format: i16, bytes: &[u8]) -> Result<String, String> {
    if format == formats::BINARY {
        decode_binary(oid, bytes).map_err(|err| format!("cannot decode a value of type {oid}: {err}"))
    } else {
        conn.text(bytes).map_err(|err| format!("a value of type {oid} is not in the client encoding: {err}"))
    }
}

/// check compares an observation against an assertion's expectations, returning every difference.
pub fn check(assertion: &ScriptTestAssertion, observation: &Observation) -> Vec<String> {
    let order = STATEMENTS.with(|s| s.borrow_mut().order(assertion.query));
    let mut problems = Vec::new();
    if let Expected::ClientError(expected) = &assertion.expected {
        match &observation.client_error {
            Some(actual) if actual.contains(expected) => {}
            actual => problems.push(format!("expected a client error containing {expected:?}, got {actual:?}")),
        }
        return problems;
    }
    if let Some(err) = &observation.client_error {
        problems.push(format!("client error: {err}"));
    }
    if assertion.expected_blocking {
        return problems;
    }
    match &assertion.expected {
        Expected::Error(expected) => match &observation.error {
            Some(actual) => check_diagnostic("error", expected, actual, &mut problems),
            None => problems.push(format!("expected error {}, but the statement succeeded", expected.code)),
        },
        expected => {
            if let Some(actual) = &observation.error {
                problems.push(format!("unexpected error: {} ({}) {}", actual.message, actual.code, actual.detail));
            }
            match expected {
                Expected::Rows { columns, rows, tag } => {
                    check_columns(columns, &observation.columns, &mut problems);
                    let names: Vec<&str> = columns.iter().map(|c| c.0).collect();
                    check_rows(rows, &observation.rows, &order, &names, &mut problems);
                    if *tag != observation.tag {
                        problems.push(format!("expected tag {tag:?}, got {:?}", observation.tag));
                    }
                }
                Expected::Tag(tag) => {
                    if *tag != observation.tag {
                        problems.push(format!("expected tag {tag:?}, got {:?}", observation.tag));
                    }
                    if !observation.columns.is_empty() {
                        problems.push(format!("expected no result columns, got {:?}", observation.columns));
                    }
                }
                Expected::Plan(facts) => {
                    let lines: Vec<String> =
                        observation.rows.iter().map(|row| row.first().cloned().flatten().unwrap_or_default()).collect();
                    problems.extend(crate::plan::check_facts(
                        facts,
                        &crate::plan::facts(&if lines.iter().any(|l| l.contains("├─") || l.contains("└─")) {
                            crate::plan::parse_gms(&lines)
                        } else {
                            crate::plan::parse_pg(&lines)
                        }),
                    ));
                }
                Expected::Ok | Expected::Error(_) | Expected::ClientError(_) => {}
            }
        }
    }
    if assertion.notices.len() != observation.notices.len() {
        problems.push(format!(
            "expected {} notices, got {}: {:?}",
            assertion.notices.len(),
            observation.notices.len(),
            observation.notices.iter().map(|n| n.message.as_str()).collect::<Vec<_>>()
        ));
    } else {
        for (expected, actual) in assertion.notices.iter().zip(&observation.notices) {
            check_diagnostic("notice", expected, actual, &mut problems);
        }
    }
    if !assertion.copy_to_stdout_file.is_empty() {
        match std::fs::read(testdata_dir().join(assertion.copy_to_stdout_file)) {
            Ok(expected) => {
                if observation.copy_out.as_deref() != Some(expected.as_slice()) {
                    problems.push(format!(
                        "COPY output differs from {}: got {:?}",
                        assertion.copy_to_stdout_file,
                        observation.copy_out.as_ref().map(|data| String::from_utf8_lossy(data).into_owned())
                    ));
                }
            }
            Err(err) => problems.push(format!("cannot read {}: {err}", assertion.copy_to_stdout_file)),
        }
    }
    problems
}

/// check_other_rows compares the rows a query returned against expected rows in any order, for queries that only
/// observe state, such as a wire test's checks on a separate connection.
pub fn check_other_rows(expected: &[&[Cell]], observation: &Observation) -> Vec<String> {
    let mut problems = Vec::new();
    if let Some(err) = &observation.client_error {
        problems.push(format!("client error: {err}"));
    }
    if let Some(err) = &observation.error {
        problems.push(format!("unexpected error: {} ({})", err.message, err.code));
    }
    check_rows(expected, &observation.rows, &Order::Any, &[], &mut problems);
    problems
}

/// check_diagnostic compares an error or notice field by field.
fn check_diagnostic(kind: &str, expected: &Diagnostic, actual: &ErrorFields, problems: &mut Vec<String>) {
    let mut differences = Vec::new();
    if expected.message_contains && !actual.message.contains(expand(expected.message).as_ref()) {
        differences.push(format!("message: expected to contain {:?}, got {:?}", expected.message, actual.message));
    }
    if expected.detail_contains && !actual.detail.contains(expand(expected.detail).as_ref()) {
        differences.push(format!("detail: expected to contain {:?}, got {:?}", expected.detail, actual.detail));
    }
    let mut compare = |field: &str, expected: &str, actual: &str| {
        if expand(expected) != actual {
            differences.push(format!("{field}: expected {expected:?}, got {actual:?}"));
        }
    };
    compare("severity", expected.severity, &actual.severity);
    compare("code", expected.code, &actual.code);
    if !expected.message_contains {
        compare("message", expected.message, &actual.message);
    }
    if !expected.detail_contains {
        compare("detail", expected.detail, &actual.detail);
    }
    compare("hint", expected.hint, &actual.hint);
    compare("position", &expected.position.to_string(), &actual.position.to_string());
    compare("schema", expected.schema, &actual.schema_name);
    compare("table", expected.table, &actual.table_name);
    compare("column", expected.column, &actual.column_name);
    compare("data type", expected.data_type, &actual.data_type_name);
    compare("constraint", expected.constraint, &actual.constraint_name);
    if let Some(context) = expected.context {
        compare("context", context, &actual.where_);
    }
    if !differences.is_empty() {
        problems.push(format!("{kind} differs: {}", differences.join("; ")));
    }
}

/// check_columns compares result columns.
fn check_columns(expected: &[Column], actual: &[(String, u32)], problems: &mut Vec<String>) {
    let matches = expected.len() == actual.len()
        && expected
            .iter()
            .zip(actual)
            .all(|(e, (name, oid))| e.0 == name && (e.1 == *oid || (e.1 == USER_DEFINED && *oid >= 16384)));
    if !matches {
        problems.push(format!(
            "expected columns {:?}, got {:?}",
            expected.iter().map(|c| (c.0, c.1)).collect::<Vec<_>>(),
            actual
        ));
    }
}

/// cell_matches reports whether a value matches an expected cell, binding expected OIDs to the server's.
fn cell_matches(expected: &Cell, actual: &Option<String>, bindings: &mut HashMap<u32, String>) -> bool {
    match (expected, actual) {
        (Cell::Null, None) => true,
        (Cell::Text(text), Some(value)) => expand(text) == value.as_str(),
        (Cell::Any, Some(_)) => true,
        (Cell::Approx(text), Some(value)) => match (text.parse::<f64>(), value.parse::<f64>()) {
            (Ok(e), Ok(a)) => e.is_sign_negative() == a.is_sign_negative() && e.to_bits().abs_diff(a.to_bits()) <= 2,
            _ => false,
        },
        (Cell::Oid(oid), Some(value)) => match bindings.get(oid) {
            Some(bound) => bound == value,
            None if bindings.values().any(|v| v == value) => false,
            None => {
                bindings.insert(*oid, value.clone());
                true
            }
        },
        _ => false,
    }
}

/// row_matches reports whether a row matches an expected row, keeping the OID bindings it needs only when it does.
fn row_matches(expected: &[Cell], actual: &[Option<String>], bindings: &mut HashMap<u32, String>) -> bool {
    let mut tentative = bindings.clone();
    let matches =
        expected.len() == actual.len() && expected.iter().zip(actual).all(|(e, a)| cell_matches(e, a, &mut tentative));
    if matches {
        *bindings = tentative;
    }
    matches
}

/// check_rows compares rows in an order their query allows.
fn check_rows(
    expected: &[&[Cell]],
    actual: &[Vec<Option<String>>],
    order: &Order,
    names: &[&str],
    problems: &mut Vec<String>,
) {
    let mut bindings = OID_BINDINGS.with(|b| b.borrow().clone());
    let tied = |a: &&[Cell], b: &&[Cell], keys: &[usize]| {
        keys.iter().all(|k| a.get(*k).is_some_and(|c| *c != Cell::Any && Some(c) == b.get(*k)))
    };
    let check = |expected: &[&[Cell]], actual: &[Vec<Option<String>>], keys: Option<&[usize]>| match keys {
        Some(keys) => actual.iter().all(|a| {
            keys.iter().all(
                |k| matches!((expected[0].get(*k), a.get(*k)), (Some(e), Some(a)) if cell_matches(e, a, &mut bindings)),
            )
        }),
        None => {
            let mut unmatched: Vec<&Vec<Option<String>>> = actual.iter().collect();
            expected.iter().all(|e| match unmatched.iter().position(|a| row_matches(e, a, &mut bindings)) {
                Some(index) => {
                    unmatched.swap_remove(index);
                    true
                }
                None => false,
            })
        }
    };
    let matches = order::matches(order, names, expected, actual, tied, check);
    if matches {
        OID_BINDINGS.with(|b| *b.borrow_mut() = bindings);
    } else {
        let described = match order {
            Order::Defined => "in order",
            Order::Any => "any order",
            Order::Sorted { .. } => "in sort order, ties in any order",
        };
        let mut text = format!("rows differ ({described}):\n  expected:\n");
        for row in expected {
            let _ = writeln!(text, "    {row:?}");
        }
        text.push_str("  actual:\n");
        for row in actual {
            let _ = writeln!(text, "    {row:?}");
        }
        problems.push(text);
    }
}

/// focused returns the items to run: the focused ones when any are focused, and otherwise all of them. Focus fails
/// the run in CI, where it must never be committed.
fn focused<T>(items: &[T], is_focused: impl Fn(&T) -> bool, describe: impl Fn(&T) -> String) -> Vec<&T> {
    let focus: Vec<&T> = items.iter().filter(|item| is_focused(item)).collect();
    if focus.is_empty() {
        return items.iter().collect();
    }
    if std::env::var_os("GITHUB_ACTION").is_some() {
        panic!("{} has focus set, which CI does not allow", describe(focus[0]));
    }
    focus
}

/// Capture is everything a script returned.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Capture {
    /// The error that stopped the script before its assertions ran, such as a failed setup statement.
    pub setup_error: Option<String>,
    /// The index of the setup statement that failed.
    pub setup_failed_at: Option<usize>,
    /// The observation of each assertion run, in order, repeated for each repetition.
    pub observations: Vec<Observation>,
    /// Errors from blocked statements that failed to finish.
    pub finish_errors: Vec<String>,
}

/// The environment variable naming a file that receives one JSON line per failure, for tooling.
pub const FAILURES_FILE_ENV: &str = "DOLTGRUST_FAILURES_FILE";

/// OBSERVATIONS_FILE_ENV names a file that, when set, collects every assertion's observation as a JSON line, from
/// which expectations can be taken.
pub const OBSERVATIONS_FILE_ENV: &str = "DOLTGRUST_OBSERVATIONS_FILE";

/// fields_json renders error or notice fields as JSON, as goport's captures do, without empty fields.
fn fields_json(fields: &ErrorFields) -> String {
    let mut parts = vec![
        format!("\"severity\":{}", json_string(&fields.severity)),
        format!("\"code\":{}", json_string(&fields.code)),
        format!("\"message\":{}", json_string(&fields.message)),
    ];
    for (name, text) in [
        ("detail", &fields.detail),
        ("hint", &fields.hint),
        ("schema", &fields.schema_name),
        ("table", &fields.table_name),
        ("column", &fields.column_name),
        ("data_type", &fields.data_type_name),
        ("constraint", &fields.constraint_name),
    ] {
        if !text.is_empty() {
            parts.push(format!("{}:{}", json_string(name), json_string(text)));
        }
    }
    if fields.position != 0 {
        parts.push(format!("\"position\":{}", fields.position));
    }
    format!("{{{}}}", parts.join(","))
}

/// record_observation appends an assertion's observation to the observations file, when one is set.
fn record_observation(script: &str, index: usize, observation: &Observation) {
    let Some(path) = std::env::var_os(OBSERVATIONS_FILE_ENV) else { return };
    let test = std::thread::current().name().unwrap_or("unknown").to_string();
    let columns: Vec<String> =
        observation.columns.iter().map(|(name, oid)| format!("[{},{oid}]", json_string(name))).collect();
    let rows: Vec<String> = observation
        .rows
        .iter()
        .map(|row| {
            let cells: Vec<String> = row.iter().map(|c| c.as_deref().map_or("null".to_string(), json_string)).collect();
            format!("[{}]", cells.join(","))
        })
        .collect();
    let notices: Vec<String> = observation.notices.iter().map(fields_json).collect();
    let line = format!(
        "{{\"test\":{},\"script\":{},\"assertion\":{index},\"observation\":{{\"columns\":[{}],\"rows\":[{}],\"queried\":{},\"tag\":{},\"error\":{},\"client_error\":{},\"notices\":[{}]}}}}\n",
        json_string(&test),
        json_string(script),
        columns.join(","),
        rows.join(","),
        observation.queried,
        json_string(&observation.tag),
        observation.error.as_ref().map_or("null".to_string(), fields_json),
        observation.client_error.as_deref().map_or("null".to_string(), json_string),
        notices.join(",")
    );
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        use std::io::Write as _;
        let _ = file.write_all(line.as_bytes());
    }
}

/// record_failure appends a failure to DOLTGRUST_FAILURES_FILE when it is set, naming the running test, the script, and
/// the part of the script that failed.
pub(crate) fn record_failure(script: &str, part: &str, query: &str, problems: &str) {
    let Some(path) = std::env::var_os(FAILURES_FILE_ENV) else { return };
    let test = std::thread::current().name().unwrap_or("unknown").to_string();
    let line = format!(
        "{{\"test\":{},\"script\":{},\"part\":{},\"query\":{},\"problems\":{}}}\n",
        json_string(&test),
        json_string(script),
        json_string(part),
        json_string(query),
        json_string(problems)
    );
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        use std::io::Write as _;
        let _ = file.write_all(line.as_bytes());
    }
}

/// json_string renders a JSON string literal.
fn json_string(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// run_script runs a single script against a fresh server, returning the failures.
pub fn run_script(target: &Target, script: &ScriptTest, repetitions: usize) -> Vec<String> {
    let mut failures = Vec::new();
    if script.skip.is_some() {
        return failures;
    }
    let name = match script.intent {
        "" => script.name.to_string(),
        intent => format!("{} (intent: {intent})", script.name),
    };
    OID_BINDINGS.with(|b| b.borrow_mut().clear());
    STATEMENTS.with(|s| *s.borrow_mut() = Statements::default());
    let mut session = match Session::start(target, script.database, script.server_config) {
        Ok(session) => session,
        Err(err) => {
            record_failure(script.name, "start", "", &err);
            return vec![format!("{name}: cannot start: {err}")];
        }
    };
    for (index, query) in script.set_up_script.iter().enumerate() {
        if let Err(err) = session.set_up(query) {
            record_failure(script.name, &format!("setup {index}"), query, &err);
            session.save_recording(script.name);
            return vec![
                format!("{name}: {err}"),
                format!("{}: server log:\n{}", script.name, session.server.log_tail()),
            ];
        }
    }
    let focus = focused(script.assertions, |a| a.focus, |a| format!("the assertion {:?}", a.query)).len()
        != script.assertions.len();
    for _ in 0..repetitions {
        for (index, assertion) in script.assertions.iter().enumerate() {
            if focus && !assertion.focus {
                continue;
            }
            if matches!(assertion.expected, Expected::Plan(_)) && target.is_postgres() {
                continue;
            }
            let observation = session.run(assertion);
            if observation.skipped {
                continue;
            }
            record_observation(script.name, index, &observation);
            let problems = match assertion.diverged {
                Some((_, lines)) if !target.is_postgres() => check_diverged(lines, &observation),
                _ => check(assertion, &observation),
            };
            if !problems.is_empty() {
                record_failure(script.name, &format!("assertion {index}"), assertion.query, &problems.join("\n"));
                let dolt = assertion.dolt.map_or(String::new(), |dolt| format!(" (Dolt: {dolt:?})"));
                failures.push(format!("{name} / {}{dolt}\n  {}", assertion.query, problems.join("\n  ")));
            }
        }
    }
    for err in session.finish() {
        failures.push(format!("{}: {err}", script.name));
    }
    session.save_recording(script.name);
    if !failures.is_empty() {
        failures.push(format!("{}: server log:\n{}", script.name, session.server.log_tail()));
    }
    failures
}

/// run_scripts runs scripts against the target in DOLTGRUST_TEST_TARGET, panicking with every failure.
pub fn run_scripts(scripts: &[ScriptTest]) {
    run_scripts_repeated(scripts, 1);
}

/// run_scripts_repeated runs each script's assertions the given number of times on the same connection.
pub fn run_scripts_repeated(scripts: &[ScriptTest], repetitions: usize) {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let mut failures = Vec::new();
    for script in focused(scripts, |s| s.focus, |s| format!("the script {:?}", s.name)) {
        failures.extend(run_script(&target, script, repetitions));
    }
    if !failures.is_empty() {
        panic!("{} failures:\n\n{}", failures.len(), failures.join("\n\n"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn doltgrust_role_set_up_becomes_postgres() {
        assert_eq!(
            postgres_set_up("create user if not exists 'auth_test_super' with superuser password 'p';"),
            "create user auth_test_super with superuser password 'p';"
        );
        assert_eq!(postgres_set_up("CREATE ROLE IF NOT EXISTS r1;"), "CREATE ROLE r1;");
        assert_eq!(postgres_set_up("CREATE TABLE t (a int);"), "CREATE TABLE t (a int);");
    }

    #[test]
    fn approximate_floats_match_within_two_units_in_the_last_place() {
        let mut bindings = HashMap::new();
        let mut matches = |expected: &'static str, actual: &str| {
            cell_matches(&Cell::Approx(expected), &Some(actual.to_string()), &mut bindings)
        };
        assert!(matches("1.0471975511965976", "1.0471975511965976"));
        assert!(matches("1.0471975511965976", "1.0471975511965979"));
        assert!(matches("0.5493061443340549", "0.5493061443340548"));
        assert!(!matches("1.0471975511965976", "1.047197551196599"));
        assert!(!matches("1.0471975511965976", "-1.0471975511965976"));
        assert!(!matches("1.0471975511965976", "NaN"));
        assert!(!cell_matches(&Cell::Approx("1"), &None, &mut bindings));
    }
}
