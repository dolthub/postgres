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

//! Owned state across calls (design item 13): a backend's records of a kind, each named to Postgres by a handle, its
//! slot and a 64-bit generation, which a call checks before it lends the record, and remembered by the resource owner
//! current when it was made, whose release drops the record unless its end came first.

use std::sync::Arc;

use crate::io::sealed::Sealed as _;
use crate::io::{InDrop, Io};
use crate::local::BackendLocal;
use crate::lock::IoMutex;
use crate::{Error, current, doors};

/// Owned is a kind of record a backend owns across calls: its number and its table. Its number is a byte, so it fits
/// the eight bits of a resource owner's datum that carry it.
pub trait Owned: Send + Sync + Sized + 'static {
    /// KIND is the kind's number, which a resource owner's datum carries beside the record's slot, and by which the
    /// owner's release finds the kind's table.
    const KIND: u8;

    /// table returns the kind's records.
    fn table() -> &'static Table<Self>;
}

/// Handle names a record to Postgres: its slot in its kind's table and the generation that tells it from every other
/// record the slot held, which never repeats.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Handle {
    pub slot: u32,
    pub generation: u64,
}

/// Table is a kind's records in each backend.
pub struct Table<R> {
    records: BackendLocal<Arc<IoMutex<Records<R>>>>,
}

/// Records is a backend's records of one kind, in slots an end frees and a later record reuses, and the generation
/// its next record takes. Nothing under its lock panics, so the lock is never poisoned.
struct Records<R> {
    slots: Vec<Option<Slot<R>>>,
    next: u64,
}

impl<R> Default for Records<R> {
    fn default() -> Records<R> {
        Records { slots: Vec::new(), next: 1 }
    }
}

/// Slot is a live record's place in its table: its generation, the resource owner that remembers it, and the record,
/// which a call takes out while it has it.
struct Slot<R> {
    generation: u64,
    owner: u64,
    record: Option<R>,
}

/// stale returns the error of a handle that names no live record of its kind: one ended, never made, or forged.
fn stale() -> Error {
    Error::fork(b"XX000", "a handle names no live record of its kind".to_owned())
}

/// datum returns what a resource owner remembers of a record: its slot and its kind's number.
fn datum<R: Owned>(slot: u32) -> u64 {
    u64::from(slot) | (u64::from(R::KIND) << 32)
}

impl<R> Table<R> {
    /// new returns a kind's table, with no record in any backend.
    pub const fn new() -> Table<R> {
        Table { records: BackendLocal::new() }
    }
}

impl<R> Default for Table<R> {
    fn default() -> Table<R> {
        Table::new()
    }
}

impl<R: Owned> Table<R> {
    /// make keeps a record for the backend and returns its handle, remembered by the current resource owner, whose
    /// release drops the record unless its end came first.
    pub fn make(&self, io: &mut Io, record: R) -> Result<Handle, Error> {
        let owner = current().owner_enlarge(&doors(io))?;
        self.made(io, record, owner)
    }

    /// made keeps a record for the backend, remembered by an owner that has made room for it.
    fn made(&self, io: &mut Io, record: R, owner: u64) -> Result<Handle, Error> {
        let host = current();
        let table = Arc::clone(&self.records.get());
        let handle = {
            let held = io.parts();
            let mut records = table.lock(held);
            records.slots.try_reserve(1).map_err(|_| Error::out_of_memory())?;
            let free = records.slots.iter().position(Option::is_none);
            let index = free.unwrap_or(records.slots.len());
            let slot = u32::try_from(index).map_err(|_| Error::out_of_memory())?;
            let generation = records.next;
            records.next += 1;
            let live = Some(Slot { generation, owner, record: Some(record) });
            match free {
                Some(_) => records.slots[index] = live,
                None => records.slots.push(live),
            }
            Handle { slot, generation }
        };
        host.owned_remember(owner, datum::<R>(handle.slot), release::<R>);
        Ok(handle)
    }

    /// with lends a call the record a handle names, refusing a handle that names no live record of the kind and a
    /// record another call has, as a call into Postgres that comes back into the fork would ask for one its caller
    /// holds. The record goes back to its slot after the call, unless its resource owner released it meanwhile.
    pub fn with<T>(
        &self,
        io: &mut Io,
        handle: Handle,
        call: impl FnOnce(&mut Io, &mut R) -> Result<T, Error>,
    ) -> Result<T, Error> {
        let table = Arc::clone(&self.records.get());
        let mut record = {
            let held = io.parts();
            let mut records = table.lock(held);
            let slot = live(&mut records, handle)?;
            slot.record.take().ok_or_else(|| {
                Error::fork(b"XX000", "a record of the fork's is in use by a call that comes back into it".to_owned())
            })?
        };
        let result = call(io, &mut record);
        let left = {
            let held = io.parts();
            let mut records = table.lock(held);
            match found(&mut records, handle) {
                Some(slot) => slot.record.replace(record),
                None => Some(record),
            }
        };
        drop(left);
        result
    }

    /// end drops the record a handle names and has its resource owner forget it, refusing a handle that names no
    /// live record of the kind and a record a call has.
    pub fn end(&self, io: &mut Io, handle: Handle) -> Result<(), Error> {
        let table = Arc::clone(&self.records.get());
        let (owner, record) = {
            let held = io.parts();
            let mut records = table.lock(held);
            let slot = live(&mut records, handle)?;
            if slot.record.is_none() {
                return Err(Error::fork(b"XX000", "a record of the fork's was ended by a call it lends to".to_owned()));
            }
            let owner = slot.owner;
            let freed = records.slots[handle.slot as usize].take();
            (owner, freed)
        };
        current().owned_forget(owner, datum::<R>(handle.slot));
        drop(record);
        Ok(())
    }
}

/// live returns the slot of the live record a handle names, or the error of a stale handle.
fn live<R>(records: &mut Records<R>, handle: Handle) -> Result<&mut Slot<R>, Error> {
    found(records, handle).ok_or_else(stale)
}

/// found returns the slot of the live record a handle names, or nothing for a stale handle.
fn found<R>(records: &mut Records<R>, handle: Handle) -> Option<&mut Slot<R>> {
    let slot = records.slots.get_mut(handle.slot as usize).and_then(Option::as_mut)?;
    (slot.generation == handle.generation).then_some(slot)
}

/// remembered returns the kind's number and the slot a resource owner's datum of an owned record carries, for the
/// owner's release, which finds the kind's table by its number.
pub fn remembered(datum: u64) -> (u8, u32) {
    (((datum >> 32) & 0xff) as u8, (datum & u64::from(u32::MAX)) as u32)
}

/// release is a kind's `released`, as a host that releases records by its own owners calls it.
fn release<R: Owned>(slot: u32) {
    R::table().released(slot);
}

impl<R> Table<R> {
    /// released drops the record in a slot its resource owner releases, inside an abort or at the owner's end, unless
    /// its end already did, after taking it out under the table's lock, so the record's drop runs with no lock held;
    /// it raises nothing.
    pub fn released(&self, slot: u32) {
        let Some(table) = self.records.existing() else { return };
        let mut dropping = InDrop::new();
        let freed = {
            let Some(mut records) = table.lock_in_drop(&mut dropping) else { return };
            records.slots.get_mut(slot as usize).and_then(Option::take)
        };
        drop(freed);
    }
}
