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

//! Starts the server that a test runs against: a fresh doltgrust process, or a fresh Postgres cluster copied from a
//! template, each on its own free port.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crate::ports::free_port;

/// The environment variable that selects the target server.
pub const TARGET_ENV: &str = "DOLTGRUST_TEST_TARGET";
/// How long a server may take to accept connections.
const STARTUP_TIMEOUT: Duration = Duration::from_secs(60);
/// The hint Postgres and doltgrust log when another socket holds the port they listen on.
const PORT_IN_USE_HINT: &str = "Is another postmaster already running on port";
/// How many fresh ports a server's start tries while other sockets take them first.
const PORT_TRIES: u32 = 5;

/// Target is the kind of server that tests run against.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Target {
    /// A doltgrust binary.
    Doltgrust {
        /// The path of the binary.
        binary: PathBuf,
    },
    /// A Postgres installation, whose clusters are copied from an initialized template data directory.
    Postgres {
        /// The directory holding the postgres and pg_ctl binaries.
        bin_dir: PathBuf,
        /// The template data directory.
        template: PathBuf,
    },
}

/// absolute returns a target's path made absolute, without the `\\?\` prefix canonicalizing adds on Windows, under which
/// Postgres' executables fail to find one another.
fn absolute(path: &str) -> Result<PathBuf, String> {
    let absolute = std::fs::canonicalize(path).map_err(|err| format!("invalid {TARGET_ENV} path {path}: {err}"))?;
    Ok(match absolute.to_str().and_then(|text| text.strip_prefix(r"\\?\")) {
        Some(plain) if !plain.starts_with(r"UNC\") => PathBuf::from(plain),
        _ => absolute,
    })
}

impl Target {
    /// from_env reads the target from DOLTGRUST_TEST_TARGET, which is either "doltgrust:<binary>" or
    /// "postgres:<bin dir>:<template data dir>".
    pub fn from_env() -> Result<Target, String> {
        let value = std::env::var(TARGET_ENV).map_err(|_| format!("{TARGET_ENV} is not set"))?;
        Target::parse(&value)
    }

    /// parse parses a target description, making its paths absolute since servers run in their own directories.
    pub fn parse(value: &str) -> Result<Target, String> {
        if let Some(binary) = value.strip_prefix("doltgrust:") {
            return Ok(Target::Doltgrust { binary: absolute(binary)? });
        }
        if let Some(rest) = value.strip_prefix("postgres:")
            && let [bin_dir, template] = std::env::split_paths(rest).collect::<Vec<_>>().as_slice()
        {
            let (bin_dir, template) = (bin_dir.to_string_lossy(), template.to_string_lossy());
            return Ok(Target::Postgres { bin_dir: absolute(&bin_dir)?, template: absolute(&template)? });
        }
        Err(format!("invalid {TARGET_ENV}: {value}"))
    }

    /// is_postgres reports whether the target is a real Postgres.
    pub fn is_postgres(&self) -> bool {
        matches!(self, Target::Postgres { .. })
    }
}

/// Server is a running server, which is stopped and deleted when dropped.
pub struct Server {
    /// The port the server listens on at 127.0.0.1.
    pub port: u16,
    child: Child,
    /// The command that starts the server's process.
    command: Command,
    directory: PathBuf,
    stop: Option<Command>,
    pg_isready: Option<PathBuf>,
}

impl Server {
    /// start starts a fresh server of the target kind. The extra configuration is YAML appended to a doltgrust
    /// config file, of which Postgres is given only the `system_variables`.
    pub fn start(target: &Target, extra_config: &str) -> Result<Server, String> {
        Server::start_with(target, extra_config, &system_variables(extra_config))
    }

    /// start_with_prepared_transactions starts a fresh server of the target kind with `max_prepared_transactions` at
    /// 10, as Postgres' regression and isolation suites need for their tests of prepared transactions.
    pub fn start_with_prepared_transactions(target: &Target) -> Result<Server, String> {
        let config = "system_variables:
  max_prepared_transactions: 10
";
        Server::start(target, config)
    }

    /// start_tls starts a fresh server of the target kind that also accepts TLS, using the PEM certificate chain and
    /// private key files.
    pub fn start_tls(target: &Target, cert: &Path, key: &Path) -> Result<Server, String> {
        let config = format!("  tls_cert: {}\n  tls_key: {}\n", cert.display(), key.display());
        let settings = [
            "ssl=on".to_string(),
            format!("ssl_cert_file={}", cert.display()),
            format!("ssl_key_file={}", key.display()),
        ];
        Server::start_with(target, &config, &settings)
    }

    /// start_with starts a fresh server with YAML appended to a doltgrust config file, or with these settings for
    /// Postgres.
    fn start_with(target: &Target, extra_config: &str, postgres_settings: &[String]) -> Result<Server, String> {
        let mut tries = 1;
        loop {
            match Server::start_on_free_port(target, extra_config, postgres_settings) {
                Err(err) if err.contains(PORT_IN_USE_HINT) && tries < PORT_TRIES => tries += 1,
                result => return result,
            }
        }
    }

    /// start_on_free_port starts a fresh server on a port that was free when it was checked, which another socket may
    /// take before the server binds it.
    fn start_on_free_port(target: &Target, extra_config: &str, postgres_settings: &[String]) -> Result<Server, String> {
        let directory = fresh_directory()?;
        let port = free_port()?;
        let result = match target {
            Target::Doltgrust { binary } => start_doltgrust(binary, &directory, port, extra_config),
            Target::Postgres { bin_dir, template } => {
                start_postgres(bin_dir, template, &directory, port, postgres_settings)
            }
        };
        let started = result.and_then(|(mut command, stop)| {
            let child = command
                .spawn()
                .map_err(|err| format!("cannot start {}: {err}", command.get_program().to_string_lossy()))?;
            Ok((child, command, stop))
        });
        let (child, command, stop) = match started {
            Ok(started) => started,
            Err(err) => {
                let _ = std::fs::remove_dir_all(&directory);
                return Err(err);
            }
        };
        let pg_isready = match target {
            Target::Postgres { bin_dir, .. } => Some(bin_dir.join("pg_isready")),
            Target::Doltgrust { .. } => None,
        };
        let mut server = Server { port, child, command, directory, stop, pg_isready };
        server.wait_until_listening(0)?;
        Ok(server)
    }

    /// wait_until_listening waits until the server has logged that it listens on its port more often than the
    /// `previous` times before it started, so that another server that took the port first is never mistaken for it,
    /// and then until it accepts connections and has finished starting up, as pg_isready waits for `PQPING_OK`.
    fn wait_until_listening(&mut self, previous: usize) -> Result<(), String> {
        let deadline = Instant::now() + STARTUP_TIMEOUT;
        loop {
            if self.listening_logged() > previous && self.is_ready() {
                return Ok(());
            }
            if let Ok(Some(status)) = self.child.try_wait() {
                return Err(format!("server exited during startup with {status}: {}", self.log_tail()));
            }
            if Instant::now() > deadline {
                return Err(format!("server did not start listening: {}", self.log_tail()));
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// is_ready reports whether the server accepts connections and has finished starting up: pg_isready's answer for
    /// Postgres, and for doltgrust the answer to a startup packet, which is not ready while it refuses the connection
    /// as starting up (SQLSTATE 57P03), as libpq's `PQping` reports `PQPING_REJECT`.
    fn is_ready(&self) -> bool {
        let Some(pg_isready) = &self.pg_isready else { return ping(self.port) };
        connect(self.port).is_some()
            && Command::new(pg_isready)
                .args(["-q", "-h", "127.0.0.1", "-p", &self.port.to_string()])
                .status()
                .is_ok_and(|status| status.success())
    }

    /// listening_logged returns how many times the server's log says it listens on its port.
    fn listening_logged(&self) -> usize {
        let log = std::fs::read_to_string(self.directory.join("server.log")).unwrap_or_default();
        log.matches(&format!("listening on IPv4 address \"127.0.0.1\", port {}\n", self.port)).count()
    }

    /// log_tail returns the end of the server's log.
    pub fn log_tail(&self) -> String {
        let log = std::fs::read_to_string(self.directory.join("server.log")).unwrap_or_default();
        let start = log.len().saturating_sub(2000);
        log[log.floor_char_boundary(start)..].to_string()
    }

    /// directory returns the server's working directory, which holds its data and log.
    pub fn directory(&self) -> &Path {
        &self.directory
    }

    /// pid returns the ID of the server's process.
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// is_running reports whether the server's process has not exited.
    pub fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    /// restart stops the server if it still runs and starts it again in its directory, keeping its data.
    pub fn restart(&mut self) -> Result<(), String> {
        self.halt();
        let previous = self.listening_logged();
        self.child = self.command.spawn().map_err(|err| format!("cannot restart the server: {err}"))?;
        self.wait_until_listening(previous)
    }

    /// halt stops the server's process.
    pub fn halt(&mut self) {
        match self.stop.as_mut() {
            Some(stop) => {
                let _ = stop.stdout(Stdio::null()).stderr(Stdio::null()).status();
                let _ = self.child.wait();
            }
            None => {
                let _ = self.child.kill();
                let _ = self.child.wait();
            }
        }
    }
}

impl Drop for Server {
    /// drop implements the interface Drop by stopping the server and deleting its directory.
    fn drop(&mut self) {
        self.halt();
        let _ = std::fs::remove_dir_all(&self.directory);
    }
}

/// TEST_TIME_ZONE is the time zone a test server's new data directory takes, the one the Postgres template that tests'
/// expectations are recorded against was made in, so that times print alike on every machine.
const TEST_TIME_ZONE: &str = "America/Los_Angeles";

/// start_doltgrust returns the command that starts a doltgrust binary with a config file that listens on the port and
/// on a Unix-domain socket in its directory and turns `fsync` and `wal_init_zero` off, as `start_postgres` starts
/// Postgres, so that a fresh server neither syncs nor zero-fills its first WAL segment before it listens. An extra
/// configuration with `system_variables` of its own gets the settings among them. The testdata directory is copied
/// into its working directory, so that relative file paths in statements resolve.
fn start_doltgrust(
    binary: &Path,
    directory: &Path,
    port: u16,
    extra_config: &str,
) -> Result<(Command, Option<Command>), String> {
    let socket_directory = directory.display().to_string().replace('\'', "''");
    let settings = format!("  fsync: off\n  wal_init_zero: off\n  unix_socket_directories: '{socket_directory}'\n");
    let config = match extra_config.lines().any(|line| line == "system_variables:") {
        true => format!(
            "log_level: warn\n\nlistener:\n  host: 127.0.0.1\n  port: {port}\n{}",
            extra_config.replacen("system_variables:\n", &format!("system_variables:\n{settings}"), 1)
        ),
        false => format!(
            "log_level: warn\n\nsystem_variables:\n{settings}\nlistener:\n  host: 127.0.0.1\n  port: {port}\n{extra_config}"
        ),
    };
    let config_path = directory.join("config.yaml");
    std::fs::write(&config_path, config).map_err(|err| err.to_string())?;
    let data_dir = directory.join("data");
    std::fs::create_dir_all(&data_dir).map_err(|err| err.to_string())?;
    let testdata = crate::script::testdata_dir();
    if testdata.is_dir() {
        copy_dir(&testdata, &directory.join("testdata")).map_err(|err| format!("cannot copy testdata: {err}"))?;
    }
    let log = std::fs::File::create(directory.join("server.log")).map_err(|err| err.to_string())?;
    let mut command = Command::new(binary);
    command
        .arg("--config")
        .arg(&config_path)
        .arg("--data-dir")
        .arg(&data_dir)
        .current_dir(directory)
        .env_remove("DOLTGRUST_DATA_DIR")
        .env("TZ", TEST_TIME_ZONE)
        .stdout(log.try_clone().map_err(|err| err.to_string())?)
        .stderr(log);
    Ok((command, None))
}

/// postgres_command returns a command that runs one of Postgres' programs with the locale every platform's Postgres
/// starts under, since a postmaster without `LC_ALL` refuses to start on macOS.
pub(crate) fn postgres_command(program: impl AsRef<std::ffi::OsStr>) -> Command {
    let mut command = Command::new(program);
    command.env("LC_ALL", "en_US.UTF-8");
    command
}

/// start_postgres copies the template into a new data directory and returns the commands that start a postmaster on
/// the port with the extra settings and stop it.
fn start_postgres(
    bin_dir: &Path,
    template: &Path,
    directory: &Path,
    port: u16,
    settings: &[String],
) -> Result<(Command, Option<Command>), String> {
    let data_dir = directory.join("data");
    copy_dir(template, &data_dir).map_err(|err| format!("cannot copy {}: {err}", template.display()))?;
    let log = std::fs::File::create(directory.join("server.log")).map_err(|err| err.to_string())?;
    let mut command = postgres_command(bin_dir.join("postgres"));
    for setting in settings {
        command.arg("-c").arg(setting);
    }
    command
        .arg("-D")
        .arg(&data_dir)
        .arg("-p")
        .arg(port.to_string())
        .arg("-c")
        .arg("listen_addresses=127.0.0.1")
        .arg("-c")
        .arg(format!("unix_socket_directories={}", directory.display()))
        .arg("-c")
        .arg("fsync=off")
        .arg("-c")
        .arg("wal_init_zero=off")
        .stdout(log.try_clone().map_err(|err| err.to_string())?)
        .stderr(log);
    let mut stop = Command::new(bin_dir.join("pg_ctl"));
    stop.arg("stop").arg("-D").arg(&data_dir).arg("-m").arg("immediate").arg("-w");
    Ok((command, Some(stop)))
}

/// system_variables returns the settings under a doltgrust config's `system_variables` as Postgres' `name=value`.
fn system_variables(config: &str) -> Vec<String> {
    config
        .lines()
        .skip_while(|line| *line != "system_variables:")
        .skip(1)
        .take_while(|line| line.starts_with(' '))
        .filter_map(|line| line.trim().split_once(": "))
        .map(|(name, value)| format!("{name}={value}"))
        .collect()
}

/// copy_dir copies a directory tree, keeping file permissions.
pub fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    std::fs::set_permissions(to, std::fs::metadata(from)?.permissions())?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let destination = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            copy_dir(&entry.path(), &destination)?;
        } else {
            std::fs::copy(entry.path(), destination)?;
        }
    }
    Ok(())
}

/// connect opens a TCP connection to a port on 127.0.0.1, or None when no server answers. A connection to itself,
/// which TCP's simultaneous open makes when the kernel picks the port as the connection's own while no one listens,
/// does not count. The connection closes with a reset, so that the many probes of starting servers leave no sockets
/// waiting out TIME_WAIT for later connections to collide with.
fn connect(port: u16) -> Option<TcpStream> {
    let stream = TcpStream::connect(("127.0.0.1", port)).ok()?;
    socket2::SockRef::from(&stream).set_linger(Some(Duration::ZERO)).ok()?;
    (stream.local_addr().ok()? != stream.peer_addr().ok()?).then_some(stream)
}

/// CANNOT_CONNECT_NOW is the SQLSTATE of a server that refuses connections while it starts up or shuts down.
const CANNOT_CONNECT_NOW: &str = "57P03";

/// ping sends a startup packet to a port and reports whether the server answered without refusing it as starting
/// up, libpq's `PQping` returning `PQPING_OK`.
fn ping(port: u16) -> bool {
    let Some(mut stream) = connect(port) else { return false };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let mut body = 196_608u32.to_be_bytes().to_vec();
    for field in ["user", "postgres", "database", "postgres", ""] {
        body.extend_from_slice(field.as_bytes());
        body.push(0);
    }
    let mut packet = (body.len() as u32 + 4).to_be_bytes().to_vec();
    packet.extend_from_slice(&body);
    if stream.write_all(&packet).is_err() {
        return false;
    }
    let mut header = [0u8; 5];
    if stream.read_exact(&mut header).is_err() {
        return false;
    }
    if header[0] != b'E' {
        return true;
    }
    let len = u32::from_be_bytes([header[1], header[2], header[3], header[4]]) as usize;
    let mut fields = vec![0u8; len.saturating_sub(4)];
    if stream.read_exact(&mut fields).is_err() {
        return false;
    }
    let code = fields
        .split(|&b| b == 0)
        .find_map(|field| field.strip_prefix(b"C"))
        .map(|code| String::from_utf8_lossy(code).into_owned());
    code.as_deref() != Some(CANNOT_CONNECT_NOW)
}

/// fresh_directory creates a new empty directory for a server. Its path is short, since Postgres limits the length
/// of its socket path.
fn fresh_directory() -> Result<PathBuf, String> {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let base = std::env::temp_dir().join("dgt");
    std::fs::create_dir_all(&base).map_err(|err| err.to_string())?;
    let name = format!("{}-{}", std::process::id(), COUNTER.fetch_add(1, Ordering::Relaxed));
    let directory = base.join(name);
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(&directory).map_err(|err| err.to_string())?;
    Ok(directory)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_targets_paths_are_absolute_without_a_verbatim_prefix() {
        let dir = std::env::temp_dir();
        let path = absolute(&dir.to_string_lossy()).unwrap();
        assert!(path.is_absolute());
        assert!(!path.to_string_lossy().starts_with(r"\\?\"), "{}", path.display());
        assert_eq!(std::fs::canonicalize(&path).unwrap(), std::fs::canonicalize(&dir).unwrap());
    }
}
