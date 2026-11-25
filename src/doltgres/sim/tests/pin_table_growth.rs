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

//! The node cache's pin table when an allocation that grows it fails (design item 14's S5): the pin is refused with
//! Postgres' out-of-memory error, the table is as it was, and every pin is still released. It runs alone in a process
//! of one thread, without the test harness, since it fails every allocation of its process while the table grows.

use host::cache::{self, Placement, Read};
use host::io::Io;

/// main runs the case and panics where it fails.
fn main() {
    let _process = sim::process();
    sim::install();
    sim::size_shared(&[2, 0, 0, 0, 0]);
    let io = &mut Io::testing();
    let key = cache::key(1, &[1; cache::ADDRESS], &[0; cache::LOCATOR]);
    sim::as_actor(1, || {
        let mut held = Vec::with_capacity(64);
        match cache::install(io, Placement::Server, &key, 100, |bytes| bytes.fill(1)).expect("an install failed") {
            Read::Cached(pinned) => held.push(pinned),
            Read::Uncached(_) => panic!("an install found no frame"),
        }
        let refused = loop {
            assert!(held.len() < 64, "the pin table never grew");
            pg::fail_allocations(true);
            let got = cache::get(io, Placement::Server, &key);
            pg::fail_allocations(false);
            match got {
                Ok(Some(pinned)) => held.push(pinned),
                Ok(None) => panic!("a pinned node was gone"),
                Err(error) => break error,
            }
        };
        assert_eq!(refused.sqlstate(), b"53200");
        assert_eq!(sim::pins(&key), Some(1), "a refused pin changed the frame's pins");
        let again = cache::get(io, Placement::Server, &key).expect("a pin after a refused one failed");
        assert_eq!(&*again.expect("a pinned node was gone"), &[1; 100][..]);
        drop(held);
        assert_eq!(sim::pins(&key), Some(0), "a pin stayed after a failed growth of the table");
    });
}
