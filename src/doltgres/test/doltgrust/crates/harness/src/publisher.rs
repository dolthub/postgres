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

//! A Postgres server that publishes to the subscriptions logical replication tests make: a fresh data directory
//! started through `pg_ctl`, which every platform's Postgres accepts, with `wal_level = logical` and SCRAM
//! authentication for user `postgres`.

use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::ports::free_port;
use crate::server::postgres_command;

/// PG_BIN_ENV names the directory of the Postgres binaries the publisher runs, `pg_config --bindir` when unset.
pub const PG_BIN_ENV: &str = "DOLTGRUST_PG_BIN";

/// PASSWORD is the password of the publisher's `postgres` user.
pub const PASSWORD: &str = "secret";

/// Publisher is a running publisher, which is stopped and deleted when dropped.
pub struct Publisher {
    /// The port the publisher listens on at 127.0.0.1.
    pub port: u16,
    bin: PathBuf,
    dir: PathBuf,
}

/// bin_dir returns the directory of the Postgres binaries, from the environment or `pg_config`.
pub fn bin_dir() -> Result<PathBuf, String> {
    if let Ok(dir) = std::env::var(PG_BIN_ENV) {
        return Ok(PathBuf::from(dir));
    }
    let not_installed = || format!("Postgres is not installed (set {PG_BIN_ENV} to its binary directory)");
    let output = Command::new("pg_config").arg("--bindir").output().map_err(|_| not_installed())?;
    if !output.status.success() {
        return Err(not_installed());
    }
    Ok(PathBuf::from(String::from_utf8_lossy(&output.stdout).trim()))
}

/// run runs a command, failing with its output when it fails.
fn run(command: &mut Command) -> Result<String, String> {
    let output = command.output().map_err(|err| format!("{command:?}: {err}"))?;
    match output.status.success() {
        true => Ok(String::from_utf8_lossy(&output.stdout).into_owned()),
        false => Err(format!(
            "{command:?}: {}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        )),
    }
}

impl Publisher {
    /// start starts a fresh Postgres 18 publisher.
    pub fn start() -> Result<Publisher, String> {
        let (bin, port) = (bin_dir()?, free_port()?);
        if !run(Command::new(bin.join("postgres")).arg("--version"))?.contains("(PostgreSQL) 18.") {
            return Err(format!("{} is not Postgres 18's (set {PG_BIN_ENV} to its binary directory)", bin.display()));
        }
        let dir = std::env::temp_dir().join(format!("doltgrust-publisher-{}-{port}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).map_err(|err| err.to_string())?;
        let publisher = Publisher { port, bin, dir };
        let password = publisher.dir.join("password");
        std::fs::write(&password, format!("{PASSWORD}\n")).map_err(|err| err.to_string())?;
        let data = publisher.dir.join("data");
        run(postgres_command(publisher.bin.join("initdb"))
            .args(["-U", "postgres", "-A", "scram-sha-256", "--locale=C", "--encoding=UTF8", "-D"])
            .arg(&data)
            .arg(format!("--pwfile={}", password.display())))?;
        let conf = "wal_level = logical\nmax_replication_slots = 20\nmax_wal_senders = 20\nlisten_addresses = '127.0.0.1'\nunix_socket_directories = ''\nfsync = off\n";
        let mut settings = std::fs::read_to_string(data.join("postgresql.conf")).map_err(|err| err.to_string())?;
        settings.push_str(conf);
        std::fs::write(data.join("postgresql.conf"), settings).map_err(|err| err.to_string())?;
        let log = publisher.dir.join("server.log");
        let started = postgres_command(publisher.bin.join("pg_ctl"))
            .arg("-D")
            .arg(&data)
            .arg("-o")
            .arg(format!("-p {port}"))
            .arg("-l")
            .arg(&log)
            .args(["-w", "start"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map_err(|err| format!("pg_ctl: {err}"))?;
        match started.success() {
            true => Ok(publisher),
            false => Err(format!("the publisher did not start: {}", std::fs::read_to_string(&log).unwrap_or_default())),
        }
    }

    /// conninfo returns a connection string to the publisher's `postgres` database.
    pub fn conninfo(&self) -> String {
        format!("host=127.0.0.1 port={} user=postgres password={PASSWORD} dbname=postgres", self.port)
    }

    /// query runs SQL on the publisher with psql and returns its unaligned rows.
    pub fn query(&self, sql: &str) -> Result<String, String> {
        run(Command::new(self.bin.join("psql"))
            .env("PGPASSWORD", PASSWORD)
            .args(["-X", "-q", "-A", "-t", "-h", "127.0.0.1", "-U", "postgres", "-d", "postgres", "-p"])
            .arg(self.port.to_string())
            .arg("-c")
            .arg(sql))
    }
}

impl Drop for Publisher {
    fn drop(&mut self) {
        let _ = postgres_command(self.bin.join("pg_ctl"))
            .arg("-D")
            .arg(self.dir.join("data"))
            .args(["-m", "immediate", "-w", "stop"])
            .output();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}
