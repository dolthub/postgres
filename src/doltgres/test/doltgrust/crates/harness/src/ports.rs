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

//! The TCP ports the servers tests start listen on.

use std::io::{Read, Seek, SeekFrom, Write};
use std::net::TcpListener;
use std::ops::Range;

/// PORTS are the ports the tests take, below the range every platform hands out to listeners and connections that
/// name no port, so that no other process takes one between a test's choice and its server's listening.
const PORTS: Range<u16> = 20000..30000;

/// free_port returns a TCP port no process listens on, the next in turn among `PORTS` of every test process, which
/// take them through a counter in a locked file of the temporary directory, so that servers the tests of different
/// binaries start at once never take one port.
pub fn free_port() -> Result<u16, String> {
    let path = std::env::temp_dir().join("doltgrust-test-ports");
    let error = |err: std::io::Error| format!("{}: {err}", path.display());
    let options = std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(&path);
    let mut file = options.map_err(error)?;
    file.lock().map_err(error)?;
    let mut text = String::new();
    file.read_to_string(&mut text).map_err(error)?;
    let mut next = text.trim().parse().ok().filter(|port| PORTS.contains(port)).unwrap_or(PORTS.start);
    for _ in PORTS {
        let port = next;
        next = if next + 1 == PORTS.end { PORTS.start } else { next + 1 };
        if TcpListener::bind(("127.0.0.1", port)).is_ok() {
            file.seek(SeekFrom::Start(0)).map_err(error)?;
            file.set_len(0).map_err(error)?;
            write!(file, "{next}").map_err(error)?;
            return Ok(port);
        }
    }
    Err(format!("every port of {PORTS:?} is in use"))
}
