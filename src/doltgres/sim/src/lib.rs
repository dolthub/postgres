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

//! The simulation's host (design item 14): `host`'s seams over the real file system, for the storage crates' tests,
//! never linked into `postgres`. Each seam that changes a file first tells the storage simulation over its directory,
//! if one runs (`faults`, Doltgrust's), which keeps what a power loss would take back and fails the seam with the crash
//! or full disk its seed chose, and the clock, the process id, and unique numbers come from the simulation the thread
//! started. Compression is the server's own, through `pg`'s zstd wrappers. A test takes its capability from `io`,
//! which installs the host the first time and holds the process for the test: one backend per process, as the server
//! runs. A test runs code as one of several backends with `as_actor`, whose backend-local state is its own, or as the
//! actors of a `Run`, which the seed or a script interleaves at every door and lock.

#![forbid(unsafe_code)]

mod cache;
mod delta;
mod faults;
mod files;
#[cfg(test)]
mod owned;
mod run;
mod shared;
mod transactions;

pub use cache::{abort_owner, begin_owner, pins, refuse_next_contains, size_shared};
pub use delta::{
    delta_holds, delta_passes, delta_waiting, delta_wakes, free_slots, refuse_any_open, refuse_any_take,
    refuse_nothing, refuse_open, refuse_owned_append, refuse_record, size_delta, unfreed,
};
pub use faults::Sim;
pub use files::{
    Process, SimHost, TempDir, TestIo, as_actor, checkpointed, collecting, connect, data_dir, install, interrupt_at,
    interrupts_held, io, kept_wal_read, logged, napped, new_cids, out_of_memory_at_next_decompress, process,
    scratch_files, set_disk_space, take_logs, take_warnings, tempdir,
};
pub use pg::heap_in_use;
pub use run::{Ending, Outcome, Report, Run};
pub use shared::{index_find, index_put, index_remove};
pub use transactions::{
    Taken, abort, begin, begin_under, commit, end, historic, next_command, non_vacuumable, release, rollback_to,
    savepoint, snapshot, truncate_commit_log, unanswered,
};
