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

//! The cases of host's owned records (design item 13's Order 2a shape) over the simulation's resource owners: a
//! record carried across calls by its handle, the handles that name no live record refused, a record its owner's
//! rollback releases dropped, and a call that comes back for a record its caller holds refused.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use host::Error;
use host::io::Io;
use host::owned::{Handle, Owned, Table};

use crate::as_actor;
use crate::cache::{abort_owner, begin_owner};

/// Counted is a test's record: a value, and the count of its drops it adds to.
struct Counted {
    value: u64,
    drops: Arc<AtomicUsize>,
}

impl Drop for Counted {
    fn drop(&mut self) {
        self.drops.fetch_add(1, Ordering::Relaxed);
    }
}

/// COUNTED is the test records' table.
static COUNTED: Table<Counted> = Table::new();

impl Owned for Counted {
    const KIND: u8 = 15;

    fn table() -> &'static Table<Counted> {
        &COUNTED
    }
}

/// counted returns a record of a value and the count of its drops.
fn counted(value: u64) -> (Counted, Arc<AtomicUsize>) {
    let drops = Arc::new(AtomicUsize::new(0));
    (Counted { value, drops: Arc::clone(&drops) }, drops)
}

/// value returns the value of the record a handle names, as a later call reads it.
fn value(io: &mut Io, handle: Handle) -> Result<u64, Error> {
    COUNTED.with(io, handle, |_, record| Ok(record.value))
}

/// refused reports whether a call failed with an internal error, the error of a handle that names no live record and
/// of a record a call holds.
fn refused<T>(result: Result<T, Error>) -> bool {
    result.is_err_and(|error| error.sqlstate() == b"XX000")
}

/// A record a call makes is lent by its handle to the calls after it, which see what earlier ones changed, and its
/// end drops it once.
#[test]
fn a_record_is_carried_across_calls_by_its_handle() {
    let _process = crate::process();
    crate::install();
    let io = &mut Io::testing();
    as_actor(1, || {
        let (record, drops) = counted(7);
        let handle = COUNTED.make(io, record).unwrap();
        COUNTED
            .with(io, handle, |_, record| {
                record.value += 1;
                Ok(())
            })
            .unwrap();
        assert_eq!(value(io, handle).unwrap(), 8);
        COUNTED.end(io, handle).unwrap();
        assert_eq!(drops.load(Ordering::Relaxed), 1);
    });
}

/// A stale handle, one whose record ended, a forged one, which names a slot or a generation no record has, and a
/// duplicated one, a copy of a handle whose record ended through the other copy, even after a later record took the
/// slot, are each refused with an internal error, never lent another record.
#[test]
fn a_stale_forged_or_duplicated_handle_is_refused() {
    let _process = crate::process();
    crate::install();
    let io = &mut Io::testing();
    as_actor(1, || {
        let (record, _) = counted(1);
        let handle = COUNTED.make(io, record).unwrap();
        let copy = handle;
        COUNTED.end(io, handle).unwrap();
        assert!(refused(value(io, handle)), "a stale handle was lent a record");
        assert!(refused(COUNTED.end(io, copy)), "a duplicated handle ended a record twice");
        let (later, _) = counted(2);
        let later = COUNTED.make(io, later).unwrap();
        assert_eq!(later.slot, handle.slot);
        assert!(refused(value(io, copy)), "a duplicated handle was lent the slot's later record");
        assert!(refused(value(io, Handle { slot: later.slot + 1, generation: later.generation })));
        assert!(refused(value(io, Handle { slot: later.slot, generation: later.generation + 1 })));
        assert!(refused(value(io, Handle { slot: later.slot, generation: 0 })));
        assert_eq!(value(io, later).unwrap(), 2);
        COUNTED.end(io, later).unwrap();
    });
}

/// A record made after a savepoint is dropped by the rollback to it, once, and its handle is refused after; a record
/// made before the savepoint is left to its owner, and a record released while a call has it is dropped when the
/// call gives it back.
#[test]
fn a_rollback_drops_the_records_its_owner_remembers() {
    let _process = crate::process();
    crate::install();
    let io = &mut Io::testing();
    as_actor(1, || {
        let (outer, outer_drops) = counted(1);
        let outer = COUNTED.make(io, outer).unwrap();
        begin_owner();
        let (inner, inner_drops) = counted(2);
        let inner = COUNTED.make(io, inner).unwrap();
        abort_owner();
        assert_eq!(inner_drops.load(Ordering::Relaxed), 1);
        assert!(refused(value(io, inner)), "a released record's handle was lent a record");
        assert_eq!(value(io, outer).unwrap(), 1);
        assert_eq!(outer_drops.load(Ordering::Relaxed), 0);
        begin_owner();
        let (lent, lent_drops) = counted(3);
        let lent = COUNTED.make(io, lent).unwrap();
        COUNTED
            .with(io, lent, |_, _| {
                abort_owner();
                Ok(())
            })
            .unwrap();
        assert_eq!(lent_drops.load(Ordering::Relaxed), 1);
        assert!(refused(value(io, lent)));
        COUNTED.end(io, outer).unwrap();
        assert_eq!(outer_drops.load(Ordering::Relaxed), 1);
    });
}

/// A call that comes back for the record its caller holds, to read it or to end it, is refused with an internal
/// error, and the record is its caller's again after.
#[test]
fn a_call_that_comes_back_for_a_record_its_caller_holds_is_refused() {
    let _process = crate::process();
    crate::install();
    let io = &mut Io::testing();
    as_actor(1, || {
        let (record, drops) = counted(5);
        let handle = COUNTED.make(io, record).unwrap();
        let (read, ended) = COUNTED
            .with(io, handle, |io, _| Ok((refused(value(io, handle)), refused(COUNTED.end(io, handle)))))
            .unwrap();
        assert!(read && ended, "a call that came back got the record its caller holds");
        assert_eq!(value(io, handle).unwrap(), 5);
        COUNTED.end(io, handle).unwrap();
        assert_eq!(drops.load(Ordering::Relaxed), 1);
    });
}
