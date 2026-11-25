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

//! The node cache (design item 14's S5): decoded tree nodes every backend shares, and a backend's own for its
//! temporary trees, each keyed by its packs' location and its address and read in place through a pin. A pin lends
//! its bytes only within the entry that took or resumed it, and what crosses calls is a `Kept`, which lends none. The
//! backend's pin table is the one record of each pin, so whichever of its drop and its resource owner's release comes
//! first unpins it, and the other does nothing.

use std::marker::PhantomData;
use std::ops::Deref;
use std::sync::Arc;

use crate::io::sealed::Sealed as _;
use crate::io::{InDrop, Io, IoDoors};
use crate::local::BackendLocal;
use crate::lock::IoMutex;
use crate::{Error, Host, current, doors};

/// ADDRESS is the length of a node's address.
pub const ADDRESS: usize = 32;

/// LOCATOR is the length of a node's locator, where a copy of it is stored, as a chunk's trailer holds one.
pub const LOCATOR: usize = 14;

/// KEY is the length of a node's key: its packs' location, its address, then its locator.
pub const KEY: usize = size_of::<u64>() + ADDRESS + LOCATOR;

/// Key is a node's key in a cache, which only `key` makes.
pub struct Key([u8; KEY]);

impl Key {
    /// bytes returns the key's bytes, for the host that looks it up.
    pub fn bytes(&self) -> &[u8; KEY] {
        &self.0
    }
}

/// key returns a node's key: its packs' location, its address, and the locator of the stored copy the node is decoded
/// from, since the address names only a chunk's hashed bytes and each stored copy has a trailer of its own.
pub fn key(location: u64, address: &[u8; ADDRESS], locator: &[u8; LOCATOR]) -> Key {
    let mut key = [0; KEY];
    let (at, rest) = key.split_at_mut(size_of::<u64>());
    let (of, stored) = rest.split_at_mut(ADDRESS);
    at.copy_from_slice(&location.to_le_bytes());
    of.copy_from_slice(address);
    stored.copy_from_slice(locator);
    Key(key)
}

/// CLASSES are a node cache's frame sizes, smallest first, each with its share of the cache's bytes in hundredths:
/// a node takes the smallest frame that holds it, and most nodes are near trees' 8 KiB target. Tuning the shares is a
/// performance-stage item.
pub const CLASSES: [(usize, usize); 5] = [(8 << 10, 40), (16 << 10, 40), (32 << 10, 12), (64 << 10, 6), (128 << 10, 2)];

/// frames returns how many frames of each class a cache of a size holds.
pub fn frames(bytes: usize) -> [usize; CLASSES.len()] {
    CLASSES.map(|(size, share)| bytes / 100 * share / size)
}

/// class returns the class of the smallest frame that holds a node's bytes, or None for bytes no frame holds.
pub fn class(len: usize) -> Option<usize> {
    CLASSES.iter().position(|&(size, _)| len <= size)
}

/// Placement is where a node cache lives: the one every backend shares, or the backend's own for temporary trees.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Placement {
    /// Server is the cache in shared memory, which every backend reads.
    Server,
    /// Backend is the backend's own cache, for its temporary trees.
    Backend,
}

/// Bytes is a cached node's bytes as the host maps them: in place in its memory, or held while it lends them.
pub enum Bytes {
    /// Mapped is a frame's bytes in the server's memory, which no one changes while the frame is pinned.
    Mapped(&'static [u8]),
    /// Lent is a frame's bytes the simulation keeps alive for as long as they are lent.
    Lent(Arc<[u8]>),
}

impl Deref for Bytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Bytes::Mapped(bytes) => bytes,
            Bytes::Lent(bytes) => bytes,
        }
    }
}

/// Found is a frame a lookup or an install pinned once in shared memory, with its bytes.
pub struct Found {
    /// frame is the frame's number in its cache.
    pub frame: u32,
    /// bytes are the frame's bytes.
    pub bytes: Bytes,
}

/// Pinned is a cached node pinned for as long as it lives, whose bytes it lends within the entry that took or resumed
/// it, which its resource owner outlasts; it is neither sent nor copied.
pub struct Pinned<'io> {
    pin: Pin,
    bytes: Bytes,
    entry: PhantomData<(&'io (), *const ())>,
}

impl Deref for Pinned<'_> {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.bytes
    }
}

impl Pinned<'_> {
    /// keep gives the pin to state that lives across calls, as a cursor keeps its node's between fetches.
    pub fn keep(self) -> Kept {
        Kept { pin: self.pin, bytes: self.bytes }
    }
}

/// Kept is a pin kept across calls, under the resource owner that took it. It lends no bytes, since that owner may
/// release the pin between calls: `resume` gives the pinned node back in a later entry while the pin lasts.
pub struct Kept {
    pin: Pin,
    bytes: Bytes,
}

impl Kept {
    /// resume returns the pinned node in the running entry, or None once the pin's resource owner released it, when
    /// its node is read again by its key.
    pub fn resume<'io>(self, io: &mut Io) -> Option<Pinned<'io>> {
        let live = {
            let held = io.parts();
            self.pin.table.lock(held).live(self.pin.slot, self.pin.generation)
        };
        live.then(|| Pinned { pin: self.pin, bytes: self.bytes, entry: PhantomData })
    }
}

/// Pin is a pin's record: the host that pinned the frame, the backend's pin table, and the pin's slot there with the
/// generation that tells it from a later pin in the same slot. Whichever of its drop and its resource owner's release
/// comes first unpins the frame.
struct Pin {
    host: &'static dyn Host,
    table: Arc<IoMutex<Pins>>,
    slot: u32,
    generation: u32,
}

impl Drop for Pin {
    fn drop(&mut self) {
        let mut dropping = InDrop::new();
        let Some(mut pins) = self.table.lock_in_drop(&mut dropping) else { return };
        let Some(live) = pins.remove(self.slot, self.generation) else { return };
        self.host.owner_forget(live.owner, datum(self.slot, self.generation));
        if !pins.holds(live.placement, live.frame) {
            self.host.cache_unpin(live.placement, live.frame);
        }
    }
}

/// Read is a node an install gave back: pinned in its cache, or in memory of its own when no frame was free.
pub enum Read<'io> {
    /// Cached is the node pinned in its cache.
    Cached(Pinned<'io>),
    /// Uncached is the node in memory of its own, which no frame could take without waiting.
    Uncached(Vec<u8>),
}

impl Deref for Read<'_> {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Read::Cached(pinned) => pinned,
            Read::Uncached(bytes) => bytes,
        }
    }
}

/// get returns the cached node a key names, pinned, or None when no frame holds it whole.
pub fn get<'io>(io: &mut Io, placement: Placement, key: &Key) -> Result<Option<Pinned<'io>>, Error> {
    let host = current();
    let owner = host.owner_enlarge(&doors(io))?;
    let table = reserve(io)?;
    let Some(found) = host.cache_get(&doors(io), placement, key)? else { return Ok(None) };
    Ok(Some(pin(io, host, table, owner, placement, found)))
}

/// install puts a node of `len` bytes in its cache under its key and returns it pinned there, or pinned where another
/// backend put it meanwhile, or in memory of its own when every frame that could take it is in use. `fill` writes
/// every one of the node's bytes, which hold anything before it does, once, unless another backend's frame is
/// returned.
pub fn install<'io>(
    io: &mut Io,
    placement: Placement,
    key: &Key,
    len: usize,
    mut fill: impl FnMut(&mut [u8]),
) -> Result<Read<'io>, Error> {
    let host = current();
    let owner = host.owner_enlarge(&doors(io))?;
    let table = reserve(io)?;
    match host.cache_install(&doors(io), placement, key, len, &mut fill)? {
        Some(found) => Ok(Read::Cached(pin(io, host, table, owner, placement, found))),
        None => {
            let mut bytes = vec![0; len];
            fill(&mut bytes);
            Ok(Read::Uncached(bytes))
        }
    }
}

/// contains reports whether a cache's table names a key, whether or not its frame is filled yet, for a reader
/// deciding whether to ask the operating system for a node ahead of reading it. It pins nothing and counts as no use,
/// so a frame only asked about goes to the clock as one never touched.
pub fn contains(io: &mut Io, placement: Placement, key: &Key) -> Result<bool, Error> {
    current().cache_contains(&doors(io), placement, key)
}

/// make_local fixes the size of the backend's own cache, as its first write of a temporary tree does, Postgres'
/// `InitLocalBuffers`; until then the backend caches no temporary node.
pub fn make_local(io: &mut impl IoDoors, bytes: usize) -> Result<(), Error> {
    current().cache_make_local(&doors(io), bytes)
}

/// released unpins the pin a resource owner remembered, unless its drop already did; it is the owner's release, which
/// runs inside an abort, so it neither allocates nor raises.
pub fn released(remembered: u64) {
    let (slot, generation) = ((remembered & u64::from(u32::MAX)) as u32, (remembered >> 32) as u32);
    let Some(table) = PINS.existing() else { return };
    let mut dropping = InDrop::new();
    let Some(mut pins) = table.lock_in_drop(&mut dropping) else { return };
    let Some(host) = pins.host else { return };
    let Some(live) = pins.remove(slot, generation) else { return };
    if !pins.holds(live.placement, live.frame) {
        host.cache_unpin(live.placement, live.frame);
    }
}

/// release_all unpins every pin the backend still holds, for its exit, which runs after its resource owners'
/// release has unpinned what they remembered.
pub fn release_all() {
    let Some(table) = PINS.existing() else { return };
    let mut dropping = InDrop::new();
    let Some(mut pins) = table.lock_in_drop(&mut dropping) else { return };
    let Some(host) = pins.host else { return };
    for slot in 0..pins.entries.len() {
        let Some(live) = pins.entries.get_mut(slot).and_then(|entry| entry.live.take()) else { continue };
        if !pins.holds(live.placement, live.frame) {
            host.cache_unpin(live.placement, live.frame);
        }
    }
}

/// PINS is the backend's pin table, shared with each of its pins.
static PINS: BackendLocal<Arc<IoMutex<Pins>>> = BackendLocal::new();

/// Pins is a backend's pin table: an entry per pin, in slots a removal clears and a later pin reuses, so a removal
/// never allocates, and the host that pinned them. A frame the backend pins more than once is pinned once in shared
/// memory, which the entries themselves tell. Nothing under its lock panics, so the lock is never poisoned and the
/// table never rebuilt under live pins.
#[derive(Default)]
struct Pins {
    entries: Vec<Entry>,
    host: Option<&'static dyn Host>,
}

/// Entry is a slot of the pin table: its generation, and its pin while it lives.
#[derive(Default)]
struct Entry {
    generation: u32,
    live: Option<Live>,
}

/// Live is a pin in the table: its frame and the resource owner that remembered it.
#[derive(Clone, Copy)]
struct Live {
    placement: Placement,
    frame: u32,
    owner: u64,
}

impl Pins {
    /// add records a pin, in the room `reserve` made, and returns its slot and generation, and whether the backend
    /// already pinned its frame.
    fn add(&mut self, placement: Placement, frame: u32, owner: u64) -> (u32, u32, bool) {
        let already = self.holds(placement, frame);
        let slot = match self.entries.iter().position(|entry| entry.live.is_none()) {
            Some(slot) => slot,
            None => {
                self.entries.push(Entry::default());
                self.entries.len() - 1
            }
        };
        let entry = &mut self.entries[slot];
        entry.generation = entry.generation.wrapping_add(1);
        entry.live = Some(Live { placement, frame, owner });
        (slot as u32, entry.generation, already)
    }

    /// remove removes a live pin's entry and returns it, or nothing when the entry is gone or its slot reused.
    fn remove(&mut self, slot: u32, generation: u32) -> Option<Live> {
        let entry = self.entries.get_mut(slot as usize)?;
        if entry.generation != generation {
            return None;
        }
        entry.live.take()
    }

    /// live reports whether a pin's entry is still in the table.
    fn live(&self, slot: u32, generation: u32) -> bool {
        self.entries.get(slot as usize).is_some_and(|entry| entry.generation == generation && entry.live.is_some())
    }

    /// holds reports whether any of the backend's pins is of a frame.
    fn holds(&self, placement: Placement, frame: u32) -> bool {
        self.entries
            .iter()
            .any(|entry| entry.live.is_some_and(|live| (live.placement, live.frame) == (placement, frame)))
    }
}

/// reserve returns the backend's pin table with room for one more pin, before its frame is pinned, so the pin is
/// recorded without failing once it is taken; a failed allocation leaves the table as it was.
fn reserve(io: &mut Io) -> Result<Arc<IoMutex<Pins>>, Error> {
    let table = Arc::clone(&PINS.get());
    let grown = {
        let held = io.parts();
        table.lock(held).entries.try_reserve(1)
    };
    grown.map_err(|_| Error::out_of_memory())?;
    Ok(table)
}

/// pin records a frame the host pinned in the backend's table and with the resource owner that has room for it,
/// giving back the shared pin when the backend already held the frame.
fn pin<'io>(
    io: &mut Io,
    host: &'static dyn Host,
    table: Arc<IoMutex<Pins>>,
    owner: u64,
    placement: Placement,
    found: Found,
) -> Pinned<'io> {
    let (slot, generation, already) = {
        let held = io.parts();
        let mut pins = table.lock(held);
        pins.host = Some(host);
        pins.add(placement, found.frame, owner)
    };
    if already {
        host.cache_unpin(placement, found.frame);
    }
    host.owner_remember(owner, datum(slot, generation));
    Pinned { pin: Pin { host, table, slot, generation }, bytes: found.bytes, entry: PhantomData }
}

/// datum is what a resource owner remembers of a pin: its slot and generation.
fn datum(slot: u32, generation: u32) -> u64 {
    u64::from(slot) | (u64::from(generation) << 32)
}

#[cfg(test)]
mod tests {
    use super::{ADDRESS, KEY, LOCATOR, key};

    /// A key holds its packs' location, its whole address, and its whole locator: two addresses that differ only in
    /// their last byte, one address at two locations, and one address stored at two locators that differ only in
    /// their last byte, have different keys.
    #[test]
    fn a_key_holds_its_location_its_whole_address_and_its_whole_locator() {
        let (one, mut other) = ([7; ADDRESS], [7; ADDRESS]);
        other[ADDRESS - 1] = 8;
        let (stored, mut copy) = ([3; LOCATOR], [3; LOCATOR]);
        copy[LOCATOR - 1] = 4;
        assert_ne!(key(1, &one, &stored).bytes(), key(1, &other, &stored).bytes());
        assert_ne!(key(1, &one, &stored).bytes(), key(2, &one, &stored).bytes());
        assert_ne!(key(1, &one, &stored).bytes(), key(1, &one, &copy).bytes());
        assert_eq!(key(1, &one, &stored).bytes()[8..KEY - LOCATOR], one);
        assert_eq!(key(1, &one, &stored).bytes()[KEY - LOCATOR..], stored);
    }
}
