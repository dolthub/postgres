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

//! Asking a server process to stop as Postgres' signals do: the signal itself on Unix, and the signal pipe Postgres'
//! `pgkill` writes to on Windows.

use std::process::Child;

/// signal sends a signal to the child.
#[cfg(unix)]
fn signal(child: &Child, signal: nix::sys::signal::Signal) -> Result<(), String> {
    let pid = nix::unistd::Pid::from_raw(child.id() as i32);
    nix::sys::signal::kill(pid, signal).map_err(|e| e.to_string())
}

/// signal sends a signal to the child through its signal pipe, numbered as Postgres numbers signals on Windows, as
/// Postgres' `pgkill` does.
#[cfg(windows)]
fn signal(child: &Child, signal: u8) -> Result<(), String> {
    use std::io::{Read, Write};
    const ERROR_PIPE_BUSY: i32 = 231;
    let name = format!(r"\\.\pipe\pgsignal_{}", child.id());
    for _ in 0..100 {
        let mut pipe = match std::fs::OpenOptions::new().read(true).write(true).open(&name) {
            Ok(pipe) => pipe,
            Err(e) if e.raw_os_error() == Some(ERROR_PIPE_BUSY) => {
                std::thread::sleep(std::time::Duration::from_millis(10));
                continue;
            }
            Err(e) => return Err(e.to_string()),
        };
        pipe.write_all(&[signal]).map_err(|e| e.to_string())?;
        let mut answer = [0u8; 1];
        return match pipe.read_exact(&mut answer) {
            Ok(()) if answer[0] == signal => Ok(()),
            Ok(()) => Err(format!("signal {signal} was answered with {}", answer[0])),
            Err(e) if e.kind() == std::io::ErrorKind::BrokenPipe => Ok(()),
            Err(e) => Err(e.to_string()),
        };
    }
    Err(format!("signal pipe {name} stayed busy"))
}

/// interrupt asks the child to stop, like pressing Ctrl-C.
pub fn interrupt(child: &Child) -> Result<(), String> {
    #[cfg(unix)]
    return signal(child, nix::sys::signal::Signal::SIGINT);
    #[cfg(windows)]
    signal(child, 2)
}

/// terminate asks the child to stop gracefully.
pub fn terminate(child: &Child) -> Result<(), String> {
    #[cfg(unix)]
    return signal(child, nix::sys::signal::Signal::SIGTERM);
    #[cfg(windows)]
    signal(child, 15)
}
