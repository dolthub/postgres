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

//! The simulation's shared memory: `host`'s shared records kept in the test process, each part plain Rust, so the
//! storage crates run the code they run over Postgres' shared memory. A kind is laid out at its first use, a pool
//! with its count of records and a word for each in the directory, which changes only through `host`'s plain
//! functions, as the server's does, and a kind of words with a record for each process that took its own, whose
//! atomics are its words.
//! Under a run (`run`), a take of a lock, flag, or the epoch lock another actor
//! holds waits until the run hands the part back; outside one, the simulation runs one actor at a time, so the
//! holder would never release it while the taker waits, and the take is refused as the hang it would be.

use std::mem::{Discriminant, discriminant};
use std::sync::{Mutex, MutexGuard, PoisonError};

use host::Error;
use host::delta::Page;
use host::shared::{Form, Kind, Limits, Looked, Op, word};

use crate::run::{self, Part};

/// PROCESSES is the number of processes the simulated server's pools are counted for.
const PROCESSES: usize = 64;

/// RELATION_LOCKS is how many relation locks the simulated server can grant at once: Postgres' default of 64 for
/// each of its processes.
const RELATION_LOCKS: usize = 64 * PROCESSES;

/// Lock is a record's lock: its exclusive holder, or the holders sharing it.
#[derive(Default)]
struct Lock {
    exclusive: Option<u32>,
    shared: Vec<u32>,
}

/// Array is one of a record's arrays: its count and its name, which a slots array uses, and its entries.
struct Array {
    count: u64,
    name: u64,
    entries: Vec<Vec<u8>>,
}

/// Record is a record's parts.
struct Record {
    locks: Vec<Lock>,
    flags: Vec<Option<u32>>,
    atomics: Vec<u64>,
    blobs: Vec<Vec<u8>>,
    arrays: Vec<Array>,
}

/// Pool is a kind's records: a pool's, each with a word in the directory, a single record, or a record for each
/// process.
struct Pool {
    /// id is the kind's shape's id.
    id: u64,
    /// form is how the kind keeps its records.
    form: Discriminant<Form>,
    /// first is the directory's slot of the pool's first record.
    first: usize,
    /// maxes gives each blob its largest size.
    maxes: &'static [usize],
    /// hand is the number among the pool's records at which the next make's sweep starts.
    hand: usize,
    /// unfreed counts an aged pool's records no free pass has freed, kept at each change of a record's word, so a
    /// run's check reads it without walking the pool (bugs row 181).
    unfreed: usize,
    records: Vec<Record>,
    /// procs gives each record of a kind of words the process that has it, or nothing once that process exited,
    /// unless the kind is kept.
    procs: Vec<Option<u32>>,
    /// kept is whether an exit leaves the words of a kind of words.
    kept: bool,
}

/// Role is what a process is to a record it has a handle of.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Role {
    /// Owner is the record's maker or claimant.
    Owner,
    /// Holder is a process that opened the record, with the number of its handles of it.
    Holder(u32),
    /// Reclaim is a reclaim looking at the record, with the word its take stored.
    Reclaim(u64),
}

/// Handle is a record a process has a handle of.
struct Handle {
    proc: u32,
    /// slot is the record's slot in the directory.
    slot: usize,
    role: Role,
}

/// Shared is the process's simulated shared memory.
struct Shared {
    /// pools are the kinds laid out so far.
    pools: Vec<Pool>,
    /// words are the directory's words, one for each pooled or aged record.
    words: Vec<u64>,
    /// aged are the aged kinds' pools, each listed before the first listed kind its records name.
    aged: Vec<usize>,
    /// handles are the records each process has handles of.
    handles: Vec<Handle>,
    /// epoch is the current epoch.
    epoch: u64,
    /// readers are the live readers, each with its process and the epoch it started at.
    readers: Vec<(u32, u64)>,
    /// epochs_held is the process that holds the epoch lock, if any.
    epochs_held: Option<u32>,
    /// stripes are the key lock's stripes held, each with its holder.
    stripes: Vec<(u32, u32)>,
    /// pushes are the key lock's stripes' push counts, each with its stripe, a stripe never released having none.
    pushes: Vec<(u32, u64)>,
    /// holds are the key lock's stripes' hold-back flags set, each with the process that set it.
    holds: Vec<(u32, u32)>,
    /// tuples are the tuple locks held, each with its holder.
    tuples: Vec<(host::TupleLock, u32)>,
    /// index is the pools' indexes, each entry a kind's id, a key, and the location the index holds for it.
    index: Vec<(u64, Vec<u8>, u64)>,
}

/// FRESH is shared memory as the server starts it.
const FRESH: Shared = Shared {
    pools: Vec::new(),
    words: Vec::new(),
    aged: Vec::new(),
    handles: Vec::new(),
    epoch: 1,
    readers: Vec::new(),
    epochs_held: None,
    stripes: Vec::new(),
    pushes: Vec::new(),
    holds: Vec::new(),
    tuples: Vec::new(),
    index: Vec::new(),
};

/// SHARED is the process's simulated shared memory.
static SHARED: Mutex<Shared> = Mutex::new(FRESH);

/// shared returns the simulated shared memory.
fn shared() -> MutexGuard<'static, Shared> {
    SHARED.lock().unwrap_or_else(PoisonError::into_inner)
}

/// internal returns the error of a seam used against its contract, which the server raises as an internal error.
fn internal(message: &str) -> Error {
    Error::fork(b"XX000", message.to_owned())
}

/// proc returns the running process's number: its actor's after the process's one backend, which is 0.
pub fn proc() -> u32 {
    crate::files::running_actor().map_or(0, |actor| u32::try_from(actor + 1).expect("actors are numbered below 2^32"))
}

/// place returns the place of a record: its pool's number, then its number among the pool's records.
fn place(pool: usize, index: usize) -> u64 {
    (pool as u64) << 32 | index as u64
}

/// placed returns a place's pool and its record's number among the pool's records.
fn placed(place: u64) -> (usize, usize) {
    ((place >> 32) as usize, place as u32 as usize)
}

impl Shared {
    /// pool returns the number of a kind's pool, laying the kind out at its first use, and refusing a shape used
    /// before in another form.
    fn pool(&mut self, kind: &Kind) -> Result<usize, Error> {
        if let Some(at) = self.pools.iter().position(|pool| pool.id == kind.id) {
            return match self.pools[at].form == discriminant(&kind.form) {
                true => Ok(at),
                false => Err(internal("a shape is used in two forms")),
            };
        }
        let limits =
            Limits { processes: PROCESSES, delta_pages: crate::delta::slots(), relation_locks: RELATION_LOCKS };
        let (count, run) = match kind.form {
            Form::Pool(count) => (count(&limits), 0),
            Form::Single { run } => (1, run(&limits)),
            Form::Aged { count, .. } => (count(&limits), 0),
            Form::Process { .. } => (0, 0),
        };
        let array =
            |&(entries, width): &(usize, usize)| Array { count: 0, name: 0, entries: vec![vec![0; width]; entries] };
        let record = || Record {
            locks: (0..kind.locks).map(|_| Lock::default()).collect(),
            flags: vec![None; kind.flags],
            atomics: vec![0; kind.atomics + run],
            blobs: vec![Vec::new(); kind.blobs.len()],
            arrays: kind.arrays.iter().map(array).collect(),
        };
        let first = self.words.len();
        if let Form::Pool(_) | Form::Aged { .. } = kind.form {
            self.words.resize(first + count, 0);
        }
        if let Form::Aged { names, .. } = kind.form {
            let before = self.aged.iter().position(|&pool| names.contains(&self.pools[pool].id));
            self.aged.insert(before.unwrap_or(self.aged.len()), self.pools.len());
        }
        self.pools.push(Pool {
            id: kind.id,
            form: discriminant(&kind.form),
            first,
            maxes: kind.blobs,
            hand: 0,
            unfreed: 0,
            records: (0..count).map(|_| record()).collect(),
            procs: Vec::new(),
            kept: matches!(kind.form, Form::Process { kept: true }),
        });
        Ok(self.pools.len() - 1)
    }

    /// slot returns a location's slot in the directory, refusing one outside a pool's slots, which only a forged
    /// location or another kind's has.
    fn slot(&self, pool: usize, location: u64) -> Result<usize, Error> {
        let (first, count) = (self.pools[pool].first, self.pools[pool].records.len());
        match usize::try_from(word::slot(location)) {
            Ok(slot) if slot >= first && slot < first + count => Ok(slot),
            _ => Err(internal("a location names no record of its shape's pool")),
        }
    }

    /// spot returns the slot in the directory of a pooled record at a place.
    fn spot(&self, place: u64) -> usize {
        let (pool, index) = placed(place);
        self.pools[pool].first + index
    }

    /// record returns the record at a place.
    fn record(&mut self, place: u64) -> &mut Record {
        let (pool, index) = placed(place);
        &mut self.pools[pool].records[index]
    }

    /// take removes a process's handle of a slot in a role, reporting the role it had, or nothing when it has none.
    fn take(&mut self, proc: u32, slot: usize, like: impl Fn(Role) -> bool) -> Option<Role> {
        let at =
            self.handles.iter().position(|handle| handle.proc == proc && handle.slot == slot && like(handle.role))?;
        Some(self.handles.swap_remove(at).role)
    }

    /// oldest returns the oldest epoch a live reader started at, or the current epoch.
    fn oldest(&self) -> u64 {
        self.readers.iter().map(|&(_, epoch)| epoch).min().unwrap_or(self.epoch)
    }

    /// store writes a record's word, keeping its aged pool's count of records no free pass has freed.
    fn store(&mut self, slot: usize, word: u64) {
        let was = std::mem::replace(&mut self.words[slot], word);
        let aged = self.aged.iter().copied().find(|&pool| {
            let pool = &self.pools[pool];
            (pool.first..pool.first + pool.records.len()).contains(&slot)
        });
        if let Some(pool) = aged {
            let unfreed = &mut self.pools[pool].unfreed;
            *unfreed = *unfreed + usize::from(unfreed_word(word)) - usize::from(unfreed_word(was));
        }
    }

    /// give_up gives up every record a process has a handle of, and then zeroes its words and frees its slots of
    /// them, as its exit does, so the list of the processes holds none that has ended; a kept kind's words and slot
    /// stay as they are.
    fn give_up(&mut self, proc: u32) {
        let (left, kept): (Vec<_>, Vec<_>) = self.handles.drain(..).partition(|handle| handle.proc == proc);
        self.handles = kept;
        for handle in left {
            let next = match handle.role {
                Role::Owner => word::give_up(self.words[handle.slot]),
                Role::Holder(_) => word::close(self.words[handle.slot]),
                Role::Reclaim(_) => word::unreclaim(self.words[handle.slot]),
            };
            self.store(handle.slot, next);
        }
        for pool in self.pools.iter_mut().filter(|pool| !pool.kept) {
            if let Some(at) = pool.procs.iter().position(|&owner| owner == Some(proc)) {
                pool.records[at].atomics.fill(0);
                pool.procs[at] = None;
            }
        }
    }
}

/// make takes a free slot of a kind's pool for a record the running process owns, its parts as a new record's, or
/// answers nothing when every slot is taken.
pub fn make(kind: &Kind) -> Result<Option<u64>, Error> {
    let me = proc();
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let (first, count, start) = (shared.pools[pool].first, shared.pools[pool].records.len(), shared.pools[pool].hand);
    for step in 0..count {
        let index = (start + step) % count;
        let Some(owned) = word::make(shared.words[first + index]) else { continue };
        shared.store(first + index, owned);
        shared.pools[pool].hand = (index + 1) % count;
        let record = &mut shared.pools[pool].records[index];
        record.atomics.fill(0);
        record.blobs.iter_mut().for_each(Vec::clear);
        record.arrays.iter_mut().flat_map(|array| &mut array.entries).for_each(|entry| entry.fill(0));
        shared.handles.push(Handle { proc: me, slot: first + index, role: Role::Owner });
        return Ok(Some(place(pool, index)));
    }
    Ok(None)
}

/// publish publishes a record the running process owns and returns its location.
pub fn publish(place: u64) -> u64 {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    shared.take(me, slot, |role| role == Role::Owner);
    let published = word::publish(shared.words[slot]);
    shared.store(slot, published);
    word::location(slot as u32, shared.words[slot])
}

/// claim takes the ownership of the published record of a kind at a location, or answers nothing for a location that
/// is stale or a record that isn't published.
pub fn claim(kind: &Kind, location: u64) -> Result<Option<u64>, Error> {
    let me = proc();
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let slot = shared.slot(pool, location)?;
    let Some(claimed) = word::claim(shared.words[slot], location) else { return Ok(None) };
    shared.store(slot, claimed);
    shared.handles.push(Handle { proc: me, slot, role: Role::Owner });
    Ok(Some(place(pool, slot - shared.pools[pool].first)))
}

/// free frees a record the running process owns.
pub fn free(place: u64) {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    shared.take(me, slot, |role| role == Role::Owner);
    let freed = word::free(shared.words[slot]);
    shared.store(slot, freed);
}

/// give_up gives up a record the running process still owns, and does nothing for one it published or freed.
pub fn give_up(place: u64) {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    if shared.take(me, slot, |role| role == Role::Owner).is_some() {
        let given = word::give_up(shared.words[slot]);
        shared.store(slot, given);
    }
}

/// open opens the published record of a kind at a location, or answers nothing for a location that is stale or a
/// record that isn't published. A record the running process already holds is counted in its own handle alone.
pub fn open(kind: &Kind, location: u64) -> Result<Option<u64>, Error> {
    let me = proc();
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let slot = shared.slot(pool, location)?;
    let place = place(pool, slot - shared.pools[pool].first);
    let Some(opened) = word::open(shared.words[slot], location) else { return Ok(None) };
    let mine =
        |handle: &&mut Handle| handle.proc == me && handle.slot == slot && matches!(handle.role, Role::Holder(_));
    if let Some(Handle { role: Role::Holder(count), .. }) = shared.handles.iter_mut().find(mine) {
        *count += 1;
        return Ok(Some(place));
    }
    shared.store(slot, opened);
    shared.handles.push(Handle { proc: me, slot, role: Role::Holder(1) });
    Ok(Some(place))
}

/// close closes one of the running process's handles of a record it opened, which leaves the record's holders at
/// the last. A process that holds no handle of the record panics, since only a handle moved from the process that
/// opened it, a test's bug, closes one.
pub fn close(place: u64) {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    let held = |handle: &Handle| handle.proc == me && handle.slot == slot && matches!(handle.role, Role::Holder(_));
    let Some(at) = shared.handles.iter().position(held) else {
        panic!("process {me} closes a record it holds no handle of, one another process opened");
    };
    if let Role::Holder(count) = &mut shared.handles[at].role {
        *count -= 1;
        if *count == 0 {
            shared.handles.swap_remove(at);
            let closed = word::close(shared.words[slot]);
            shared.store(slot, closed);
        }
    }
}

/// reclaim looks at the published record of a kind at a location for a reclaim, taking it when no process holds it
/// and no other reclaim is looking at it.
pub fn reclaim(kind: &Kind, location: u64) -> Result<Looked, Error> {
    let me = proc();
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let slot = shared.slot(pool, location)?;
    Ok(match word::reclaim(shared.words[slot], location) {
        word::Look::Take(taken) => {
            shared.store(slot, taken);
            shared.handles.push(Handle { proc: me, slot, role: Role::Reclaim(taken) });
            Looked::Mine(place(pool, slot - shared.pools[pool].first))
        }
        word::Look::Theirs => Looked::Theirs,
        word::Look::Used => Looked::Used,
    })
}

/// reclaimed frees a record the running process's reclaim took if its slot still holds the word the take stored, and
/// otherwise gives the record up.
pub fn reclaimed(place: u64) -> bool {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    let Some(Role::Reclaim(taken)) = shared.take(me, slot, |role| matches!(role, Role::Reclaim(_))) else {
        return false;
    };
    let freed = shared.words[slot] == taken;
    let next = if freed { word::reclaimed(taken) } else { word::unreclaim(shared.words[slot]) };
    shared.store(slot, next);
    freed
}

/// unreclaim gives up a record the running process's reclaim still has, and does nothing for one it freed or gave
/// up.
pub fn unreclaim(place: u64) {
    let me = proc();
    let mut shared = shared();
    let slot = shared.spot(place);
    if shared.take(me, slot, |role| matches!(role, Role::Reclaim(_))).is_some() {
        let unclaimed = word::unreclaim(shared.words[slot]);
        shared.store(slot, unclaimed);
    }
}

/// sweep lists the published records of a kind's pool: each one's location and the value of one of its atomics,
/// read between two loads of the slot's word, for the slots those two words show published at one generation. Each
/// of a slot's three steps is a yield point, so a run can free the slot and make it again between any two.
pub fn sweep(kind: &Kind, atomic: usize) -> Result<Vec<(u64, u64)>, Error> {
    let (pool, first, count) = {
        let mut shared = shared();
        let pool = shared.pool(kind)?;
        (pool, shared.pools[pool].first, shared.pools[pool].records.len())
    };
    let mut listed = Vec::new();
    for index in 0..count {
        run::yield_point();
        let loaded = shared().words[first + index];
        if !word::published(loaded) {
            continue;
        }
        run::yield_point();
        let value = shared().pools[pool].records[index].atomics[atomic];
        run::yield_point();
        if word::swept(loaded, shared().words[first + index]) {
            listed.push((word::location((first + index) as u32, loaded), value));
        }
    }
    Ok(listed)
}

/// single returns the place of the one record of a kind.
pub fn single(kind: &Kind) -> Result<u64, Error> {
    Ok(place(shared().pool(kind)?, 0))
}

/// lock takes a record's lock.
pub fn lock(place: u64, lock: usize, exclusive: bool) -> Result<(), Error> {
    let me = proc();
    loop {
        {
            let mut shared = shared();
            let held = &mut shared.record(place).locks[lock];
            if held.exclusive == Some(me) || held.shared.contains(&me) {
                return Err(internal("a backend took a shared lock it already holds"));
            }
            if held.exclusive.is_none() && !(exclusive && !held.shared.is_empty()) {
                match exclusive {
                    true => held.exclusive = Some(me),
                    false => held.shared.push(me),
                }
                return Ok(());
            }
        }
        if !run::wait(Part::Lock { place, lock, exclusive }) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's lock would never end");
        }
    }
}

/// unlock releases a record's lock the running process holds.
pub fn unlock(place: u64, lock: usize) {
    let me = proc();
    let mut shared = shared();
    let held = &mut shared.record(place).locks[lock];
    if held.exclusive == Some(me) {
        held.exclusive = None;
    } else {
        held.shared.retain(|&holder| holder != me);
    }
}

/// key_lock takes the key lock's stripes exclusively, in the order given, waiting for each another process holds, and
/// refusing one the running process holds, as an `LWLock` taken twice would hang.
pub fn key_lock(stripes: &[u32]) -> Result<(), Error> {
    let me = proc();
    for &stripe in stripes {
        loop {
            {
                let mut shared = shared();
                match shared.stripes.iter().find(|&&(held, _)| held == stripe) {
                    Some(&(_, holder)) if holder == me => {
                        return Err(internal("a backend took a key lock's stripe it already holds"));
                    }
                    Some(_) => {}
                    None => {
                        shared.stripes.push((stripe, me));
                        break;
                    }
                }
            }
            if !run::wait(Part::Stripe { stripe }) {
                panic!("the simulation runs one actor at a time, so a wait for another actor's stripe would never end");
            }
        }
    }
    Ok(())
}

/// key_unlock releases stripes the running process holds, moving each one's push count when it pushed under them.
pub fn key_unlock(stripes: &[u32], pushed: bool) {
    let me = proc();
    let mut shared = shared();
    shared.stripes.retain(|&(held, holder)| holder != me || !stripes.contains(&held));
    for &stripe in stripes.iter().filter(|_| pushed) {
        match shared.pushes.iter_mut().find(|(at, _)| *at == stripe) {
            Some((_, count)) => *count += 1,
            None => shared.pushes.push((stripe, 1)),
        }
    }
}

/// key_hold sets a stripe's hold-back flag for the running process unless another process's is set, and reports
/// whether the running process's is.
pub fn key_hold(stripe: u32) -> bool {
    let me = proc();
    let mut shared = shared();
    match shared.holds.iter().find(|&&(at, _)| at == stripe) {
        Some(&(_, holder)) => holder == me,
        None => {
            shared.holds.push((stripe, me));
            true
        }
    }
}

/// key_release clears a stripe's hold-back flag if the running process set it.
pub fn key_release(stripe: u32) {
    let me = proc();
    shared().holds.retain(|&(at, holder)| at != stripe || holder != me);
}

/// key_held_wait waits while another process's hold-back flag of a stripe is set; a cancel or a termination ends it.
pub fn key_held_wait(stripe: u32) -> Result<(), Error> {
    while !free_for(Part::Held { stripe }, proc()) {
        if !run::wait(Part::Held { stripe }) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's hold would never end");
        }
    }
    Ok(())
}

/// key_count reads a stripe's push count.
pub fn key_count(stripe: u32) -> u64 {
    shared().pushes.iter().find(|&&(at, _)| at == stripe).map_or(0, |&(_, count)| count)
}

/// lock_tuple takes a row's tuple lock, waiting while another process holds a conflicting one, or with `wait` false
/// answering at once whether it took it.
pub fn lock_tuple(tuple: host::TupleLock, wait: bool) -> Result<bool, Error> {
    let me = proc();
    loop {
        {
            let mut shared = shared();
            if !shared.tuples.iter().any(|&(held, holder)| holder != me && conflicts(held, tuple)) {
                shared.tuples.push((tuple, me));
                return Ok(true);
            }
        }
        if !wait {
            return Ok(false);
        }
        if !run::wait(Part::Tuple { tuple }) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's tuple lock would never end");
        }
    }
}

/// unlock_tuple releases one take of a row's tuple lock the running process holds.
pub fn unlock_tuple(tuple: host::TupleLock) {
    let me = proc();
    let mut shared = shared();
    if let Some(at) = shared.tuples.iter().position(|&(held, holder)| holder == me && held == tuple) {
        shared.tuples.swap_remove(at);
    }
}

/// flag_try sets a record's flag when no other actor holds it, answering whether it did.
pub fn flag_try(place: u64, flag: usize) -> Result<bool, Error> {
    let me = proc();
    let mut shared = shared();
    let held = &mut shared.record(place).flags[flag];
    match *held {
        None => {
            *held = Some(me);
            Ok(true)
        }
        Some(holder) if holder == me => Err(internal("a backend set a shared flag it already holds")),
        Some(_) => Ok(false),
    }
}

/// flag_take sets a record's flag, trying it again after each wait for its holder.
pub fn flag_take(place: u64, flag: usize) -> Result<(), Error> {
    loop {
        if flag_try(place, flag)? {
            return Ok(());
        }
        crate::files::checked()?;
        if !run::wait(Part::Flag { place, flag }) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's flag would never end")
        }
    }
}

/// flag_clear clears a record's flag.
pub fn flag_clear(place: u64, flag: usize) {
    shared().record(place).flags[flag] = None;
}

/// atomic operates on a record's atomic.
pub fn atomic(place: u64, atomic: usize, op: Op) -> u64 {
    let mut shared = shared();
    let value = &mut shared.record(place).atomics[atomic];
    let before = *value;
    match op {
        Op::Load => {}
        Op::Add(by) => *value = value.wrapping_add(by),
        Op::Exchange(expected, new) if before == expected => *value = new,
        Op::Exchange(..) => {}
    }
    before
}

/// blob_read returns the bytes of a record's blob.
pub fn blob_read(place: u64, blob: usize) -> Vec<u8> {
    shared().record(place).blobs[blob].clone()
}

/// blob_write writes the bytes of a record's blob, refusing bytes longer than the blob's largest size with nothing
/// written.
pub fn blob_write(place: u64, blob: usize, bytes: &[u8]) -> Result<(), Error> {
    let mut shared = shared();
    if bytes.len() > shared.pools[placed(place).0].maxes[blob] {
        return Err(internal("a shared blob's bytes are longer than its largest size"));
    }
    shared.record(place).blobs[blob] = bytes.to_vec();
    Ok(())
}

/// entry_read returns the bytes of an entry of a record's array, refusing an entry past the array's count.
pub fn entry_read(place: u64, array: usize, entry: usize) -> Result<Vec<u8>, Error> {
    let found = shared().record(place).arrays[array].entries.get(entry).cloned();
    found.ok_or_else(|| internal("a shared array's entry is past its count"))
}

/// entry_write writes the bytes of an entry of a record's array, refusing an entry past the array's count and bytes
/// of another length than an entry's with nothing written.
pub fn entry_write(place: u64, array: usize, entry: usize, bytes: &[u8]) -> Result<(), Error> {
    let mut shared = shared();
    let Some(held) = shared.record(place).arrays[array].entries.get_mut(entry) else {
        return Err(internal("a shared array's entry is past its count"));
    };
    if bytes.len() != held.len() {
        return Err(internal("a shared array's entry is of another length than its bytes"));
    }
    held.copy_from_slice(bytes);
    Ok(())
}

/// aged returns the number of the aged kinds laid out so far.
pub(crate) fn aged() -> u32 {
    shared().aged.len() as u32
}

/// aged_kind returns the number among the aged kinds of the kind of a shape's id, or nothing for one not laid out.
pub(crate) fn aged_kind(id: u64) -> Option<u32> {
    let shared = shared();
    shared.aged.iter().position(|&pool| shared.pools[pool].id == id).map(|at| at as u32)
}

/// aged_pool lays an aged kind out at its first use and returns its number among the aged kinds.
pub(crate) fn aged_pool(kind: &Kind) -> Result<u32, Error> {
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    Ok(shared.aged.iter().position(|&aged| aged == pool).unwrap_or(usize::MAX) as u32)
}

/// aged_records returns the count of an aged kind's records.
pub(crate) fn aged_records(aged: u32) -> u32 {
    let shared = shared();
    shared.pools[shared.aged[aged as usize]].records.len() as u32
}

/// aged_slot returns the directory's slot of a record at an index among an aged kind's records.
fn aged_slot(shared: &Shared, aged: u32, index: u32) -> usize {
    shared.pools[shared.aged[aged as usize]].first + index as usize
}

/// aged_unfreed counts an aged kind's records no free pass has freed.
pub(crate) fn aged_unfreed(aged: u32) -> usize {
    let shared = shared();
    shared.pools[shared.aged[aged as usize]].unfreed
}

/// unfreed_word says whether a record's or a page's word is one no free pass has freed: neither free nor spent.
pub(crate) fn unfreed_word(word: u64) -> bool {
    !matches!(host::delta::page::state(word), host::delta::page::State::Free | host::delta::page::State::Spent)
}

/// aged_word loads an aged record's word.
pub(crate) fn aged_word(aged: u32, index: u32) -> u64 {
    let shared = shared();
    shared.words[aged_slot(&shared, aged, index)]
}

/// aged_swap stores an aged record's word over the one loaded, reporting whether it was still there.
pub(crate) fn aged_swap(aged: u32, index: u32, loaded: u64, word: u64) -> bool {
    let mut shared = shared();
    let slot = aged_slot(&shared, aged, index);
    let same = shared.words[slot] == loaded;
    if same {
        shared.store(slot, word);
    }
    same
}

/// aged_stamp returns an aged record's last atomic, the epoch of its retirement, to load or store.
pub(crate) fn aged_stamp(aged: u32, index: u32, stamp: Option<u64>) -> u64 {
    let mut shared = shared();
    let pool = shared.aged[aged as usize];
    let held = shared.pools[pool].records[index as usize].atomics.last_mut().expect("an aged record has its stamp");
    if let Some(stamp) = stamp {
        *held = stamp;
    }
    *held
}

/// aged_hand returns the index a sweep of an aged kind's pool starts at, and moves the hand on.
pub(crate) fn aged_hand(aged: u32) -> u32 {
    let mut shared = shared();
    let pool = shared.aged[aged as usize];
    let (hand, count) = (shared.pools[pool].hand, shared.pools[pool].records.len());
    shared.pools[pool].hand = (hand + 1) % count;
    hand as u32
}

/// aged_clear zeroes an aged record's atomics, empties its blobs, and zeroes its arrays, for its maker.
pub(crate) fn aged_clear(aged: u32, index: u32) {
    let mut shared = shared();
    let pool = shared.aged[aged as usize];
    let record = &mut shared.pools[pool].records[index as usize];
    record.atomics.fill(0);
    record.blobs.iter_mut().for_each(Vec::clear);
    record.arrays.iter_mut().flat_map(|array| &mut array.entries).for_each(|entry| entry.fill(0));
}

/// aged_place returns the place of the record at an index among an aged kind's records.
pub(crate) fn aged_place(aged: u32, index: u32) -> u64 {
    place(shared().aged[aged as usize], index as usize)
}

/// aged_pool_of returns the pool of the aged kind a number names among the aged kinds, which a later kind's layout,
/// inserted before a kind it names, never moves as it moves the numbers.
pub(crate) fn aged_pool_of(aged: u32) -> u32 {
    shared().aged[aged as usize] as u32
}

/// aged_of_pool returns the number among the aged kinds of an aged kind's pool.
pub(crate) fn aged_of_pool(pool: u32) -> u32 {
    shared().aged.iter().position(|&aged| aged == pool as usize).unwrap_or(usize::MAX) as u32
}

/// aged_at returns the number among the aged kinds and the index of the aged record at a place.
pub(crate) fn aged_at(place: u64) -> (u32, u32) {
    let (pool, index) = placed(place);
    let aged = shared().aged.iter().position(|&aged| aged == pool).unwrap_or(usize::MAX);
    (aged as u32, index as u32)
}

/// atomics returns the count of the record at a place's atomics, a single record's run included.
pub fn atomics(place: u64) -> usize {
    shared().record(place).atomics.len()
}

/// location returns the location of the record at a place: a pooled record's, or zero for a single record, which
/// has no slot and is the one record of its shape.
pub fn location(place: u64) -> u64 {
    let shared = shared();
    let (pool, index) = placed(place);
    if shared.pools[pool].form == discriminant(&Form::Single { run: |_| 0 }) {
        return 0;
    }
    if shared.aged.contains(&pool) {
        return Page::of(index as u32, shared.words[shared.pools[pool].first + index]).value();
    }
    let slot = shared.spot(place);
    word::location(slot as u32, shared.words[slot])
}

/// live reports whether the pooled record at a location still has its slot.
pub fn live(location: u64) -> bool {
    word::live(shared().words[word::slot(location) as usize], location)
}

/// gone reports whether the record of a kind that a location named no longer has its slot, refusing a location
/// outside the kind's pool.
pub fn gone(kind: &Kind, location: u64) -> Result<bool, Error> {
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let slot = shared.slot(pool, location)?;
    Ok(!word::live(shared.words[slot], location))
}

/// slots_name loads the name of a record's slots array.
pub fn slots_name(place: u64, array: usize) -> u64 {
    shared().record(place).arrays[array].name
}

/// slots_rename stores a name in a record's slots array if the array still holds the name its caller loaded,
/// reporting whether it did.
pub fn slots_rename(place: u64, array: usize, loaded: u64, name: u64) -> bool {
    let mut shared = shared();
    let held = &mut shared.record(place).arrays[array].name;
    let same = *held == loaded;
    if same {
        *held = name;
    }
    same
}

/// slots_empty zeroes the count of a record's slots array.
pub fn slots_empty(place: u64, array: usize) {
    shared().record(place).arrays[array].count = 0;
}

/// slots_claim adds an entry to a record's slots array and writes its bytes, reporting whether it did: an array
/// whose every entry is claimed takes none.
pub fn slots_claim(place: u64, array: usize, bytes: &[u8]) -> bool {
    let mut shared = shared();
    let slots = &mut shared.record(place).arrays[array];
    let Some(entry) = usize::try_from(slots.count).ok().and_then(|won| slots.entries.get_mut(won)) else {
        return false;
    };
    entry.copy_from_slice(bytes);
    slots.count += 1;
    true
}

/// slots_read returns the claimed entries of a record's slots array.
pub fn slots_read(place: u64, array: usize) -> Vec<Vec<u8>> {
    let mut shared = shared();
    let slots = &shared.record(place).arrays[array];
    slots.entries.iter().take(slots.count as usize).cloned().collect()
}

/// mine returns the place of the running process's slot of a kind of words, which its first use makes or takes
/// from a process that exited.
pub fn mine(kind: &Kind) -> Result<u64, Error> {
    let me = proc();
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let words = &mut shared.pools[pool];
    if let Some(at) = words.procs.iter().position(|&owner| owner == Some(me)) {
        return Ok(place(pool, at));
    }
    if let Some(at) = words.procs.iter().position(|owner| owner.is_none()) {
        words.procs[at] = Some(me);
        return Ok(place(pool, at));
    }
    let atomics = vec![0; kind.atomics];
    words.records.push(Record { locks: Vec::new(), flags: Vec::new(), atomics, blobs: Vec::new(), arrays: Vec::new() });
    words.procs.push(Some(me));
    Ok(place(pool, words.records.len() - 1))
}

/// word_store stores a word of the running process's slot at a place.
pub fn word_store(place: u64, word: usize, value: u64) {
    shared().record(place).atomics[word] = value;
}

/// word_load loads a word of a process's slot of a kind of words, which is zero for a process that took no slot. The
/// simulation numbers its processes without a bound, so it refuses no number.
pub fn word_load(kind: &Kind, proc: u32, word: usize) -> Result<u64, Error> {
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let words = &shared.pools[pool];
    Ok(words.procs.iter().position(|&owner| owner == Some(proc)).map_or(0, |at| words.records[at].atomics[word]))
}

/// words_place returns the place of a process's slot of a kind of words, refusing a process that took no slot.
pub fn words_place(kind: &Kind, proc: u32) -> Result<u64, Error> {
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    let at = shared.pools[pool].procs.iter().position(|&owner| owner == Some(proc));
    at.map(|at| place(pool, at)).ok_or_else(|| internal("a process took no slot of the kind of words"))
}

/// processes returns the numbers of the processes whose slot of a kind of words can hold a word: those that took
/// their slot, and for a kept kind those that took it and have ended, whose words stay readable.
pub fn processes(kind: &Kind) -> Result<Vec<u32>, Error> {
    let mut shared = shared();
    let pool = shared.pool(kind)?;
    Ok(shared.pools[pool].procs.iter().flatten().copied().collect())
}

/// epochs_lock takes the epoch lock.
pub fn epochs_lock() -> Result<(), Error> {
    let me = proc();
    loop {
        {
            let mut shared = shared();
            match shared.epochs_held {
                None => {
                    shared.epochs_held = Some(me);
                    return Ok(());
                }
                Some(holder) if holder == me => return Err(internal("a backend took the epoch lock it already holds")),
                Some(_) => {}
            }
        }
        if !run::wait(Part::Epochs) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's lock would never end")
        }
    }
}

/// epochs_take takes the epoch lock for a step that can't fail, a retirement of the delta's pages, which its caller
/// never holds already, and returns the current epoch.
pub(crate) fn epochs_take() -> u64 {
    let me = proc();
    loop {
        {
            let mut shared = shared();
            if shared.epochs_held.is_none() {
                shared.epochs_held = Some(me);
                return shared.epoch;
            }
        }
        if !run::wait(Part::Epochs) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's lock would never end")
        }
    }
}

/// epochs_take_now takes the epoch lock when no actor holds it, and returns the current epoch, or nothing while it is
/// held.
pub(crate) fn epochs_take_now() -> Option<u64> {
    let mut shared = shared();
    if shared.epochs_held.is_some() {
        return None;
    }
    shared.epochs_held = Some(proc());
    Some(shared.epoch)
}

/// epochs_unlock releases the epoch lock.
pub fn epochs_unlock() {
    shared().epochs_held = None;
}

/// epochs_current returns the current epoch.
pub fn epochs_current() -> u64 {
    shared().epoch
}

/// epochs_advance returns the current epoch and advances it.
pub fn epochs_advance() -> u64 {
    let mut shared = shared();
    let epoch = shared.epoch;
    shared.epoch += 1;
    epoch
}

/// epochs_oldest returns the oldest live reader's epoch, or the current one.
pub fn epochs_oldest() -> u64 {
    shared().oldest()
}

/// epochs_read starts a reader of the running process at the current epoch.
pub fn epochs_read() -> u64 {
    let me = proc();
    while shared().epochs_held.is_some_and(|holder| holder != me) && run::wait(Part::Epochs) {}
    let mut shared = shared();
    let epoch = shared.epoch;
    shared.readers.push((me, epoch));
    epoch
}

/// epochs_end ends a reader of the running process: its epoch's entry, or else, for a reader a wait at the delta's
/// budget renewed (`epochs_resumed`), the process's oldest entry newer than its epoch.
pub fn epochs_end(epoch: u64) {
    let me = proc();
    let mut shared = shared();
    let readers = &shared.readers;
    let renewed = || {
        (0..readers.len()).filter(|&at| readers[at].0 == me && readers[at].1 > epoch).min_by_key(|&at| readers[at].1)
    };
    if let Some(at) = readers.iter().position(|&reader| reader == (me, epoch)).or_else(renewed) {
        shared.readers.swap_remove(at);
    }
}

/// epochs_paused gives up the epochs of all the running process's readers for a wait at the delta's budget (the budget
/// wait text, draft 2) and returns how many it gave up, which `epochs_resumed` starts again.
pub(crate) fn epochs_paused() -> usize {
    let me = proc();
    let mut shared = shared();
    let count = shared.readers.iter().filter(|&&(reader, _)| reader == me).count();
    shared.readers.retain(|&(reader, _)| reader != me);
    count
}

/// epochs_resumed starts again, at the current epoch, the readers a wait gave up.
pub(crate) fn epochs_resumed(count: usize) {
    let me = proc();
    let mut shared = shared();
    let epoch = shared.epoch;
    shared.readers.extend(std::iter::repeat_n((me, epoch), count));
}

/// free_for reports whether a part a process waits on is free for it.
pub(crate) fn free_for(part: Part, me: u32) -> bool {
    let mut shared = shared();
    match part {
        Part::Lock { place, lock, exclusive } => {
            let held = &shared.record(place).locks[lock];
            held.exclusive.is_none() && !(exclusive && held.shared.iter().any(|&holder| holder != me))
        }
        Part::Flag { place, flag } => shared.record(place).flags[flag].is_none(),
        Part::Epochs => shared.epochs_held.is_none(),
        Part::Budget { frees } => crate::delta::frees() != frees,
        Part::Stripe { stripe } => !shared.stripes.iter().any(|&(held, _)| held == stripe),
        Part::Held { stripe } => !shared.holds.iter().any(|&(at, holder)| at == stripe && holder != me),
        Part::Ended { xid } => {
            drop(shared);
            crate::transactions::status(host::Xid(xid)).map_or(true, |status| status != host::Status::InProgress)
        }
        Part::Tuple { tuple } => !shared.tuples.iter().any(|&(held, holder)| holder != me && conflicts(held, tuple)),
    }
}

/// holders returns the processes that hold a part.
pub(crate) fn holders(part: Part) -> Vec<u32> {
    let mut shared = shared();
    match part {
        Part::Lock { place, lock, .. } => {
            let held = &shared.record(place).locks[lock];
            held.exclusive.into_iter().chain(held.shared.iter().copied()).collect()
        }
        Part::Flag { place, flag } => shared.record(place).flags[flag].into_iter().collect(),
        Part::Epochs => shared.epochs_held.into_iter().collect(),
        Part::Budget { .. } => Vec::new(),
        Part::Stripe { stripe } => {
            shared.stripes.iter().filter(|&&(held, _)| held == stripe).map(|&(_, holder)| holder).collect()
        }
        Part::Held { stripe } => {
            shared.holds.iter().filter(|&&(at, _)| at == stripe).map(|&(_, holder)| holder).collect()
        }
        Part::Ended { .. } => Vec::new(),
        Part::Tuple { tuple } => {
            shared.tuples.iter().filter(|&&(held, _)| conflicts(held, tuple)).map(|&(_, holder)| holder).collect()
        }
    }
}

/// conflicts reports whether two tuple locks conflict: the same row, in heavyweight modes that conflict, as heap's
/// `tupleLockExtraInfo` gives them (`AccessShareLock`, `RowShareLock`, `ExclusiveLock`, `AccessExclusiveLock`).
fn conflicts(held: host::TupleLock, wanted: host::TupleLock) -> bool {
    let row = |lock: host::TupleLock| (lock.database, lock.relation, lock.tid);
    let clash = matches!((held.mode.min(wanted.mode), held.mode.max(wanted.mode)), (_, 3) | (1, 2) | (2, 2));
    row(held) == row(wanted) && clash
}

/// clear_flags clears the flags a process holds, before each of its exit works and at its exit.
pub(crate) fn clear_flags(proc: u32) {
    let mut shared = shared();
    for record in shared.pools.iter_mut().flat_map(|pool| &mut pool.records) {
        for flag in record.flags.iter_mut().filter(|holder| **holder == Some(proc)) {
            *flag = None;
        }
    }
}

/// exit releases what a process's exit callbacks would: it clears the flags it holds, gives up the records it has
/// handles of, zeroes its words, and ends its readers.
pub(crate) fn exit(proc: u32) {
    crate::cache::exit(proc);
    crate::delta::exit(proc);
    clear_flags(proc);
    let mut shared = shared();
    shared.give_up(proc);
    shared.readers.retain(|&(reader, _)| reader != proc);
    shared.stripes.retain(|&(_, holder)| holder != proc);
    shared.holds.retain(|&(_, holder)| holder != proc);
    shared.tuples.retain(|&(_, holder)| holder != proc);
}

/// index_find answers the location a pool's index holds for a key, or nothing when it holds none (bugs row 405).
pub fn index_find(kind: &Kind, key: &[u8]) -> Result<Option<u64>, Error> {
    crate::run::yield_point();
    let mut shared = shared();
    shared.pool(kind)?;
    Ok(shared.index.iter().find(|(id, at, _)| *id == kind.id && at.as_slice() == key).map(|&(_, _, location)| location))
}

/// index_put sets the location a pool's index holds for a key (bugs row 405).
pub fn index_put(kind: &Kind, key: &[u8], location: u64) -> Result<(), Error> {
    crate::run::yield_point();
    let mut shared = shared();
    shared.pool(kind)?;
    match shared.index.iter_mut().find(|(id, at, _)| *id == kind.id && at.as_slice() == key) {
        Some(entry) => entry.2 = location,
        None => shared.index.push((kind.id, key.to_vec(), location)),
    }
    Ok(())
}

/// index_remove takes a key out of a pool's index when the index holds that location for it (bugs row 405).
pub fn index_remove(kind: &Kind, key: &[u8], location: u64) -> Result<(), Error> {
    crate::run::yield_point();
    let mut shared = shared();
    shared.pool(kind)?;
    shared.index.retain(|(id, at, held)| !(*id == kind.id && at.as_slice() == key && *held == location));
    Ok(())
}

/// reset starts shared memory afresh, as a crash's restart makes all of it anew.
pub(crate) fn reset() {
    crate::cache::reset();
    crate::delta::reset();
    *shared() = FRESH;
}

/// discard drops what a failed run's stuck processes left in shared memory: the locks and flags they held, their
/// handles of records, their words, their readers, and the epoch lock if one of them held it.
pub(crate) fn discard(procs: &[u32]) {
    for &proc in procs {
        crate::cache::discard(proc);
        crate::delta::exit(proc);
    }
    let mut shared = shared();
    for record in shared.pools.iter_mut().flat_map(|pool| &mut pool.records) {
        for lock in &mut record.locks {
            lock.exclusive = lock.exclusive.filter(|holder| !procs.contains(holder));
            lock.shared.retain(|holder| !procs.contains(holder));
        }
        for flag in &mut record.flags {
            *flag = flag.filter(|holder| !procs.contains(holder));
        }
    }
    for &proc in procs {
        shared.give_up(proc);
    }
    shared.readers.retain(|(reader, _)| !procs.contains(reader));
    if shared.epochs_held.is_some_and(|holder| procs.contains(&holder)) {
        shared.epochs_held = None;
    }
}

/// state returns the state of the slot a location names, for the tests.
#[cfg(test)]
pub(crate) fn state(location: u64) -> word::State {
    shared().words.get(word::slot(location) as usize).map_or(word::State::Free, |&slot| word::state(slot))
}

/// free_slots returns the number of free slots in the directory, for the tests.
#[cfg(test)]
pub(crate) fn free_slots() -> usize {
    shared().words.iter().filter(|&&slot| word::state(slot) == word::State::Free).count()
}

/// aged_slots returns the directory's slots of the records of the aged kind of a shape's id, for the tests.
#[cfg(test)]
fn aged_slots(shared: &Shared, id: u64) -> std::ops::Range<usize> {
    let pool = shared.aged.iter().copied().find(|&pool| shared.pools[pool].id == id).expect("the kind is laid out");
    shared.pools[pool].first..shared.pools[pool].first + shared.pools[pool].records.len()
}

/// aged_free returns the number of free records of the aged kind of a shape's id, for the tests.
#[cfg(test)]
pub(crate) fn aged_free(id: u64) -> usize {
    let shared = shared();
    aged_slots(&shared, id)
        .filter(|&slot| host::delta::page::state(shared.words[slot]) == host::delta::page::State::Free)
        .count()
}

/// aged_walked counts, by a walk of their words, the records of the aged kind of a shape's id no free pass has freed,
/// for a test of the count a run's check reads.
#[cfg(test)]
pub(crate) fn aged_walked(id: u64) -> usize {
    let shared = shared();
    aged_slots(&shared, id).filter(|&slot| unfreed_word(shared.words[slot])).count()
}

/// age_aged stores a word in every free record of the aged kind of a shape's id, for a test of a record's last
/// generation.
#[cfg(test)]
pub(crate) fn age_aged(id: u64, word: u64) {
    let mut shared = shared();
    for slot in aged_slots(&shared, id) {
        if host::delta::page::state(shared.words[slot]) == host::delta::page::State::Free {
            shared.store(slot, word);
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::{Arc, Mutex};

    use host::io::Io;
    use host::shared::{
        self, After, ArrayAt, Atomic, BlobAt, Entries, Flag, Held, Limits, Lock, LongLock, Look, Op, Owned, Pool,
        ProcSlots, Shape, Single, Slots, Word, array, distinct, epochs, read,
    };

    use crate::run::{Ending, Outcome, Run};

    /// Test is a test's record shape: a long lock, a later short one, and a later long one, two ordered flags, an
    /// atomic, and a blob of
    /// at most 8 bytes and an entry of 4 under the short lock.
    struct Test;

    impl Shape for Test {
        const ID: u64 = 0x7e57;
        const LOCKS: usize = 3;
        const FLAGS: usize = 2;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[8];
        const ARRAYS: &'static [(usize, usize)] = &[array::<Self, KEPT>(1)];
    }

    /// LISTED are the locations `Test`'s reclaim looks at.
    static LISTED: Mutex<Vec<u64>> = Mutex::new(Vec::new());

    impl Pool for Test {
        const NAME: &'static str = "test";

        fn count(_: &Limits) -> usize {
            3
        }

        /// reclaim frees the first listed record that no process holds and whose atomic is zero.
        fn reclaim(io: &mut Io) -> Result<bool, host::Error> {
            let listed = LISTED.lock().unwrap().clone();
            let mut undecided = false;
            for location in listed {
                match shared::reclaim::<Test>(io, location)? {
                    Look::Mine(look) => {
                        if look.record().atomic::<Count>(Op::Load) != 0 {
                            continue;
                        }
                        if look.free() {
                            return Ok(true);
                        }
                        undecided = true;
                    }
                    Look::Theirs => undecided = true,
                    Look::Used => {}
                }
            }
            Ok(undecided)
        }
    }

    /// Outer is the long lock.
    struct Outer;
    impl LongLock<Test> for Outer {
        const INDEX: usize = 0;
    }

    /// Inner is the short lock, taken after the long one.
    struct Inner;
    impl Lock<Test> for Inner {
        const INDEX: usize = 1;
    }
    impl After<Outer> for Inner {}

    /// Later is the long lock taken after the first long one.
    struct Later;
    impl LongLock<Test> for Later {
        const INDEX: usize = 2;
    }
    impl After<Outer> for Later {}

    /// First is the first flag.
    struct First;
    impl Flag<Test> for First {
        const INDEX: usize = 0;
    }

    /// Second is the flag taken after the first.
    struct Second;
    impl Flag<Test> for Second {
        const INDEX: usize = 1;
    }
    impl After<First> for Second {}

    /// Count is the atomic.
    struct Count;
    impl Atomic<Test> for Count {
        const INDEX: usize = 0;
    }

    /// TABLE is the index of the blob under the short lock.
    const TABLE: usize = 0;
    impl BlobAt<TABLE> for Test {
        type Guard = Inner;
    }

    /// KEPT is the index of the entry under the short lock.
    const KEPT: usize = 0;
    impl ArrayAt<KEPT> for Test {
        type By = Entries<Inner>;
        type Entry = [u8; 4];
    }

    const _: () = assert!(distinct(&[Outer::INDEX, Inner::INDEX, Later::INDEX], Test::LOCKS));

    /// Other is a second pool, of one record with no parts.
    struct Other;

    impl Shape for Other {
        const ID: u64 = 0x7e58;
        const LOCKS: usize = 0;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 0;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Pool for Other {
        const NAME: &'static str = "other";

        fn count(_: &Limits) -> usize {
            1
        }

        fn reclaim(_: &mut Io) -> Result<bool, host::Error> {
            Ok(false)
        }
    }

    /// Lone is a single record's shape: an atomic.
    struct Lone;

    impl Shape for Lone {
        const ID: u64 = 0x7e59;
        const LOCKS: usize = 0;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Single for Lone {}

    /// Total is the single record's atomic.
    struct Total;
    impl Atomic<Lone> for Total {
        const INDEX: usize = 0;
    }

    /// Board is a single record's shape: a lock, three notes of 4 bytes under it, and two slots of 8 bytes under the
    /// long lock of a `Test` record.
    struct Board;

    impl Shape for Board {
        const ID: u64 = 0x7e5a;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 0;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[array::<Self, NOTES>(3), array::<Self, HITS>(2)];
    }

    impl Single for Board {}

    /// Pin is the board's lock.
    struct Pin;
    impl Lock<Board> for Pin {
        const INDEX: usize = 0;
    }

    /// NOTES is the index of the board's notes, under its lock.
    const NOTES: usize = 0;
    impl ArrayAt<NOTES> for Board {
        type By = Entries<Pin>;
        type Entry = [u8; 4];
    }

    /// HITS is the index of the board's slots, under the long lock of the `Test` record they name.
    const HITS: usize = 1;
    impl ArrayAt<HITS> for Board {
        type By = Slots<Test, Outer>;
        type Entry = [u8; 8];
    }

    /// Roots is a slot for each process: two words.
    struct Roots;

    impl ProcSlots for Roots {
        const ID: u64 = 0x7e5b;
        const WORDS: usize = 2;
    }

    /// Root is the slot's first word.
    struct Root;
    impl Word<Roots> for Root {
        const INDEX: usize = 0;
    }

    /// Tail is the slot's second word.
    struct Tail;
    impl Word<Roots> for Tail {
        const INDEX: usize = 1;
    }

    /// held makes a record, publishes it, and opens it, returning its location and the handle.
    fn held(io: &mut Io) -> (u64, Held<Test>) {
        let location = shared::make::<Test>(io).unwrap().publish();
        (location, shared::open::<Test>(io, location).unwrap().expect("a published record opens"))
    }

    /// mark marks a record as the one whose long lock guards the board's slots, reporting whether it did.
    fn mark(io: &mut Io, held: &Held<Test>) -> Result<bool, host::Error> {
        let record = held.record();
        let mut barrier = record.lock_long::<Outer>(io)?;
        shared::single::<Board>()?.mark::<HITS>(&mut barrier)
    }

    /// claim claims one of the board's slots for bytes through a record's long lock held shared, reporting whether
    /// it did.
    fn claim(io: &mut Io, held: &Held<Test>, bytes: &[u8; 8]) -> Result<bool, host::Error> {
        let record = held.record();
        let barrier = record.lock_long_shared::<Outer>(io)?;
        shared::single::<Board>()?.claim::<HITS>(&barrier, bytes)
    }

    /// hits reads the board's claimed slots through a record's long lock held exclusively.
    fn hits(io: &mut Io, held: &Held<Test>) -> Result<Vec<Vec<u8>>, host::Error> {
        let record = held.record();
        let mut barrier = record.lock_long::<Outer>(io)?;
        shared::single::<Board>()?.read::<HITS>(&mut barrier)
    }

    /// unmark clears the board's slots' name through a record's long lock held exclusively.
    fn unmark(io: &mut Io, held: &Held<Test>) -> Result<(), host::Error> {
        let record = held.record();
        let mut barrier = record.lock_long::<Outer>(io)?;
        shared::single::<Board>()?.unmark::<HITS>(&mut barrier)
    }

    /// io installs the simulation's host, starts shared memory afresh with nothing listed for the reclaim, and
    /// returns a test's capability.
    fn io() -> Io {
        crate::install();
        super::reset();
        LISTED.lock().unwrap().clear();
        Io::testing()
    }

    /// sqlstate returns the SQLSTATE of a result's error.
    fn sqlstate<T>(result: Result<T, host::Error>) -> Option<[u8; 5]> {
        result.err().map(|error| *error.sqlstate())
    }

    /// A blob is written in place under its lock held exclusively, taken in order under the long lock; bytes one past
    /// its largest size are refused, and the next read gives the bytes before.
    #[test]
    fn a_blob_is_written_under_its_lock() {
        let _process = crate::process();
        let io = &mut io();
        let owned: Owned<Test> = shared::make(io).unwrap();
        let record = owned.record();
        let mut outer = record.lock_long::<Outer>(io).unwrap();
        let mut long_io = outer.io();
        let mut inner = long_io.lock::<Inner>().unwrap();
        assert_eq!(inner.read_blob::<TABLE>(), b"");
        inner.write_blob::<TABLE>(b"listed").unwrap();
        assert_eq!(sqlstate(inner.write_blob::<TABLE>(b"too long!")), Some(*b"XX000"));
        assert_eq!(inner.read_blob::<TABLE>(), b"listed");
    }

    /// A later long lock is taken under a long lock, exclusively and shared, and is held: the process's second take
    /// of it is refused.
    #[test]
    fn a_later_long_lock_is_taken_under_a_long_lock() {
        let _process = crate::process();
        let (first, again) = (&mut io(), &mut Io::testing());
        let owned: Owned<Test> = shared::make(first).unwrap();
        let record = owned.record();
        let mut outer = record.lock_long::<Outer>(first).unwrap();
        let mut under = outer.io();
        let later = under.lock_long::<Later>().unwrap();
        assert_eq!(sqlstate(record.lock_long_shared::<Later>(again)), Some(*b"XX000"));
        drop(later);
        let _shared = under.lock_long_shared::<Later>().unwrap();
        assert_eq!(sqlstate(record.lock_long::<Later>(again)), Some(*b"XX000"));
    }

    /// A process's second take of a lock or flag it holds is refused as the re-entry it is.
    #[test]
    fn a_second_take_of_a_held_lock_or_flag_is_refused() {
        let _process = crate::process();
        let (first, again, third) = (&mut io(), &mut Io::testing(), &mut Io::testing());
        let owned: Owned<Test> = shared::make(first).unwrap();
        let record = owned.record();
        let mut flag = record.flag::<First>(first).unwrap();
        let mut flag_io = flag.io();
        let _second = flag_io.flag::<Second>().unwrap();
        assert_eq!(sqlstate(record.flag::<First>(again)), Some(*b"XX000"));
        let _inner = record.lock_shared::<Inner>(again).unwrap();
        assert_eq!(sqlstate(record.lock::<Inner>(third)), Some(*b"XX000"));
    }

    /// An atomic returns its value before each operation, and an exchange changes it only from the value it expects.
    #[test]
    fn an_atomic_returns_its_value_before_each_operation() {
        let _process = crate::process();
        let io = &mut io();
        let owned: Owned<Test> = shared::make(io).unwrap();
        let record = owned.record();
        assert_eq!(record.atomic::<Count>(Op::Add(5)), 0);
        assert_eq!(record.atomic::<Count>(Op::Exchange(4, 9)), 5);
        assert_eq!(record.atomic::<Count>(Op::Exchange(5, 9)), 5);
        assert_eq!(record.atomic::<Count>(Op::Load), 9);
    }

    /// A pool takes its count of makes and the next fails with an internal error naming the kind; after a free the
    /// next make succeeds at once and reads zeroed, its entry with its atomic.
    #[test]
    fn a_pool_takes_its_count_of_makes() {
        let _process = crate::process();
        let io = &mut io();
        let mut owned: Vec<Owned<Test>> = (0..3).map(|_| shared::make(io).unwrap()).collect();
        owned[0].record().atomic::<Count>(Op::Add(7));
        owned[0].record().lock::<Inner>(io).unwrap().set_entry::<KEPT>(0, b"kept").unwrap();
        let full = shared::make::<Test>(io).err().expect("a full pool refuses a make");
        assert_eq!((full.sqlstate(), full.message()), (b"XX000", "every test record is in use"));
        shared::free(owned.swap_remove(0));
        let again: Owned<Test> = shared::make(io).unwrap();
        assert_eq!(again.record().atomic::<Count>(Op::Load), 0);
        assert_eq!(again.record().lock::<Inner>(io).unwrap().entry::<KEPT>(0).unwrap(), [0; 4]);
        assert_eq!(super::free_slots(), 0);
    }

    /// A full pool whose reclaim frees an idle record makes past its count, and one whose listed records are held or
    /// hold something fails.
    #[test]
    fn a_full_pool_makes_after_its_reclaim() {
        let _process = crate::process();
        let io = &mut io();
        let locations: Vec<u64> = (0..3).map(|_| shared::make::<Test>(io).unwrap().publish()).collect();
        LISTED.lock().unwrap().extend(&locations);
        let kept = shared::open::<Test>(io, locations[0]).unwrap().expect("a published record opens");
        let busy = shared::open::<Test>(io, locations[1]).unwrap().expect("a published record opens");
        busy.record().atomic::<Count>(Op::Add(1));
        drop(busy);
        let made: Owned<Test> = shared::make(io).unwrap();
        assert_eq!(super::state(locations[2]), shared::word::State::Owned);
        assert!(shared::open::<Test>(io, locations[2]).unwrap().is_none());
        assert!(shared::make::<Test>(io).is_err());
        drop((kept, made));
    }

    /// A forged location and another pool's are refused; a stale one answers nothing.
    #[test]
    fn a_location_outside_its_pool_is_refused() {
        let _process = crate::process();
        let io = &mut io();
        let location = shared::make::<Test>(io).unwrap().publish();
        let other = shared::make::<Other>(io).unwrap().publish();
        assert_eq!(sqlstate(shared::open::<Test>(io, other)), Some(*b"XX000"));
        assert_eq!(sqlstate(shared::claim::<Other>(io, location)), Some(*b"XX000"));
        assert_eq!(sqlstate(shared::reclaim::<Test>(io, 99 << 32)), Some(*b"XX000"));
        shared::free(shared::claim::<Test>(io, location).unwrap().expect("a published record is claimed"));
        assert!(shared::open::<Test>(io, location).unwrap().is_none());
        assert!(shared::claim::<Test>(io, location).unwrap().is_none());
        assert!(matches!(shared::reclaim::<Test>(io, location).unwrap(), Look::Used));
    }

    /// A record freed while a process holds it open takes no new open, is still read by its holder, and is gone at
    /// the holder's last close; a process's second handle of it counts once.
    #[test]
    fn a_record_freed_while_held_goes_at_its_last_close() {
        let _process = crate::process();
        let io = &mut io();
        let location = shared::make::<Test>(io).unwrap().publish();
        let held = shared::open::<Test>(io, location).unwrap().expect("a published record opens");
        let twice = shared::open::<Test>(io, location).unwrap().expect("a held record opens again");
        held.record().atomic::<Count>(Op::Add(4));
        shared::free(shared::claim::<Test>(io, location).unwrap().expect("a held record is claimed"));
        assert!(shared::open::<Test>(io, location).unwrap().is_none());
        assert_eq!(held.record().atomic::<Count>(Op::Load), 4);
        drop(held);
        assert_eq!(super::state(location), shared::word::State::Condemned);
        drop(twice);
        assert_eq!(super::state(location), shared::word::State::Free);
    }

    /// A dropped owner frees a record it never published and publishes a claimed one again, where it opens.
    #[test]
    fn a_dropped_owner_gives_its_record_up() {
        let _process = crate::process();
        let io = &mut io();
        drop(shared::make::<Test>(io).unwrap());
        assert_eq!(super::free_slots(), 3);
        let location = shared::make::<Test>(io).unwrap().publish();
        drop(shared::claim::<Test>(io, location).unwrap().expect("a published record is claimed"));
        assert!(shared::open::<Test>(io, location).unwrap().is_some());
    }

    /// A reclaim frees a record only if no process opened or claimed it since it took it; a record a process holds
    /// is in use, and one a reclaim holds is another's.
    #[test]
    fn a_reclaim_frees_only_a_record_not_reached_since() {
        let _process = crate::process();
        let io = &mut io();
        let location = shared::make::<Test>(io).unwrap().publish();
        let Look::Mine(look) = shared::reclaim::<Test>(io, location).unwrap() else { panic!("the reclaim takes it") };
        assert!(matches!(shared::reclaim::<Test>(io, location).unwrap(), Look::Theirs));
        shared::open::<Test>(io, location).unwrap().expect("opens").record().atomic::<Count>(Op::Add(1));
        assert!(!look.free());
        assert_eq!(shared::open::<Test>(io, location).unwrap().expect("opens").record().atomic::<Count>(Op::Load), 1);
        let Look::Mine(look) = shared::reclaim::<Test>(io, location).unwrap() else { panic!("the reclaim takes it") };
        drop(shared::claim::<Test>(io, location).unwrap().expect("a record a reclaim looks at is claimed"));
        assert!(!look.free());
        let held = shared::open::<Test>(io, location).unwrap().expect("opens");
        assert!(matches!(shared::reclaim::<Test>(io, location).unwrap(), Look::Used));
        drop(held);
        let Look::Mine(look) = shared::reclaim::<Test>(io, location).unwrap() else { panic!("the reclaim takes it") };
        assert!(look.free());
        assert!(shared::open::<Test>(io, location).unwrap().is_none());
    }

    /// A reclaim given up on a record claimed and freed meanwhile frees the condemned slot.
    #[test]
    fn a_reclaim_given_up_frees_a_condemned_record() {
        let _process = crate::process();
        let io = &mut io();
        let location = shared::make::<Test>(io).unwrap().publish();
        let Look::Mine(look) = shared::reclaim::<Test>(io, location).unwrap() else { panic!("the reclaim takes it") };
        shared::free(shared::claim::<Test>(io, location).unwrap().expect("claims"));
        assert_eq!(super::state(location), shared::word::State::Condemned);
        assert!(!look.free());
        assert_eq!(super::state(location), shared::word::State::Free);
    }

    /// A single record is reached by its type, and a shape used as the other form is refused.
    #[test]
    fn a_single_record_is_reached_by_its_type() {
        let _process = crate::process();
        let _io = io();
        shared::single::<Lone>().unwrap().atomic::<Total>(Op::Add(3));
        assert_eq!(shared::single::<Lone>().unwrap().atomic::<Total>(Op::Load), 3);
    }

    /// A process's word stored by it is loaded by its number, a process that stored none reads zero, and a process's
    /// words read zero after it exits.
    #[test]
    fn a_processs_words_are_loaded_by_its_number() {
        let _process = crate::process();
        let io = &mut io();
        let mine = shared::mine::<Roots>(io).unwrap();
        mine.store::<Root>(7);
        mine.store::<Tail>(9);
        assert_eq!((mine.load::<Root>(), mine.load::<Tail>()), (7, 9));
        assert_eq!(shared::load::<Roots, Root>(super::proc()).unwrap(), 7);
        assert_eq!(shared::load::<Roots, Tail>(super::proc()).unwrap(), 9);
        assert_eq!(shared::load::<Roots, Root>(super::proc() + 1).unwrap(), 0);
        super::exit(super::proc());
        assert_eq!((mine.load::<Root>(), mine.load::<Tail>()), (0, 0));
    }

    /// An actor's word is loaded by another actor while it runs, with the actor's number in the list of processes,
    /// and reads zero once the actor has ended.
    #[test]
    fn an_actors_words_are_zeroed_at_its_exit() {
        let _process = crate::process();
        let _io = io();
        let mut run = Run::new(3);
        let seen = Arc::new(Mutex::new(None));
        let sees = Arc::clone(&seen);
        run.actor("a", |io| {
            let mine = shared::mine::<Roots>(io)?;
            let lone = shared::single::<Lone>()?;
            mine.store::<Tail>(41);
            lone.atomic::<Total>(Op::Add(u64::from(shared::proc_number())));
            while lone.atomic::<Total>(Op::Load) != 0 {}
            Ok(())
        });
        run.actor("b", move |_| {
            let lone = shared::single::<Lone>()?;
            let proc = loop {
                let stored = lone.atomic::<Total>(Op::Load);
                if stored != 0 {
                    break stored as u32;
                }
            };
            if !shared::processes::<Roots>()?.contains(&proc) {
                return Err(host::Error::fork(
                    b"XX000",
                    "the list of processes lacks one that took its slot".to_owned(),
                ));
            }
            *sees.lock().unwrap() = Some((proc, shared::load::<Roots, Tail>(proc)?));
            lone.atomic::<Total>(Op::Exchange(u64::from(proc), 0));
            Ok(())
        });
        assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
        let (proc, word) = seen.lock().unwrap().expect("the second actor loaded the first's word");
        assert_eq!(word, 41);
        assert_eq!(shared::load::<Roots, Tail>(proc).unwrap(), 0);
    }

    /// Stays is a kept slot for each process: one word, which an exit leaves.
    struct Stays;

    impl ProcSlots for Stays {
        const ID: u64 = 0x7e5c;
        const WORDS: usize = 1;
        const KEPT: bool = true;
    }

    /// Stay is the kept slot's word.
    struct Stay;
    impl Word<Stays> for Stay {
        const INDEX: usize = 0;
    }

    /// A kept kind's words of an actor that returned and of one that was terminated are loaded after it ended, with
    /// its number still in the kind's list of processes, while the same actors' words of a kind that isn't kept read
    /// zero and that kind's list is empty.
    #[test]
    fn a_kept_kinds_words_outlive_their_actor() {
        let _process = crate::process();
        let _io = io();
        let stored = Arc::new(Mutex::new(Vec::new()));
        let mut run = Run::new(5);
        for (name, value) in [("returns", 41), ("terminated", 43)] {
            let stores = Arc::clone(&stored);
            run.actor(name, move |io| {
                shared::mine::<Stays>(io)?.store::<Stay>(value);
                shared::mine::<Roots>(io)?.store::<Tail>(value);
                stores.lock().unwrap().push((shared::proc_number(), value));
                let lone = shared::single::<Lone>()?;
                if value == 43 {
                    loop {
                        lone.atomic::<Total>(Op::Load);
                    }
                }
                Ok(())
            });
        }
        run.script(&[("terminated", 12), ("returns", u64::MAX)]);
        run.terminate_at("terminated", 40);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!((&endings[0].1, &endings[1].1), (&Ending::Returned(Ok(())), &Ending::Terminated));
        let mut stored = std::mem::take(&mut *stored.lock().unwrap());
        stored.sort_unstable();
        let mut listed = shared::processes::<Stays>().unwrap();
        listed.sort_unstable();
        assert_eq!(listed, stored.iter().map(|&(proc, _)| proc).collect::<Vec<_>>());
        assert_eq!(stored.len(), 2);
        assert!(shared::processes::<Roots>().unwrap().is_empty());
        for (proc, value) in stored {
            assert_eq!(shared::load::<Stays, Stay>(proc).unwrap(), value);
            assert_eq!(shared::load::<Roots, Tail>(proc).unwrap(), 0);
        }
    }

    /// A run's actors that took their slots of words are in the list of the processes while they run and not after
    /// it: a second run's actor finds only itself in the list.
    #[test]
    fn a_runs_processes_leave_the_list_when_it_ends() {
        let _process = crate::process();
        let _io = io();
        let listed = Arc::new(Mutex::new(Vec::new()));
        for _ in 0..2 {
            let mut run = Run::new(5);
            let lists = Arc::clone(&listed);
            run.actor("a", move |io| {
                shared::mine::<Roots>(io)?.store::<Root>(1);
                lists.lock().unwrap().push((shared::proc_number(), shared::processes::<Roots>()?));
                Ok(())
            });
            assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
            assert!(shared::processes::<Roots>().unwrap().is_empty());
        }
        let listed = listed.lock().unwrap();
        assert_ne!(listed[0].0, listed[1].0);
        assert_eq!((&listed[0].1, &listed[1].1), (&vec![listed[0].0], &vec![listed[1].0]));
    }

    /// An entry set under its lock held exclusively is read under it shared; an index past the array's count is
    /// refused, and the entry reads as before.
    #[test]
    fn an_entry_is_set_under_its_lock() {
        let _process = crate::process();
        let io = &mut io();
        let board = shared::single::<Board>().unwrap();
        {
            let mut pin = board.lock::<Pin>(io).unwrap();
            assert_eq!(pin.entry::<NOTES>(2).unwrap(), [0; 4]);
            pin.set_entry::<NOTES>(2, b"note").unwrap();
            assert_eq!(sqlstate(pin.set_entry::<NOTES>(3, b"past")), Some(*b"XX000"));
            assert_eq!(sqlstate(pin.entry::<NOTES>(3)), Some(*b"XX000"));
        }
        let mut pin = board.lock_shared::<Pin>(io).unwrap();
        assert_eq!(pin.entry::<NOTES>(2).unwrap(), b"note");
        assert_eq!(pin.entry::<NOTES>(0).unwrap(), [0; 4]);
    }

    /// With a record marked, claims through its barrier fill the slots and the next answers false; a claim through
    /// another record's barrier, and one with no record marked, record nothing; a second mark while the record is
    /// named answers false; a read and an unmark through another record's barrier are refused; and after the unmark
    /// another record's mark succeeds and reads no slot.
    #[test]
    fn a_marked_records_claims_are_read_through_its_barrier() {
        let _process = crate::process();
        let io = &mut io();
        let board = shared::single::<Board>().unwrap();
        let ((first, x), (second, y)) = (held(io), held(io));
        assert_eq!(board.named::<HITS>(), None);
        assert!(!claim(io, &x, b"unmarked").unwrap());
        assert!(mark(io, &x).unwrap());
        assert!(!mark(io, &y).unwrap());
        assert!(!mark(io, &x).unwrap());
        assert_eq!(board.named::<HITS>(), Some(first));
        assert!(!claim(io, &y, b"another!").unwrap());
        assert!(claim(io, &x, b"hit one!").unwrap());
        assert!(claim(io, &x, b"hit two!").unwrap());
        assert!(!claim(io, &x, b"hit past").unwrap());
        assert_eq!(sqlstate(hits(io, &y)), Some(*b"XX000"));
        assert_eq!(sqlstate(unmark(io, &y)), Some(*b"XX000"));
        assert_eq!(hits(io, &x).unwrap(), [b"hit one!", b"hit two!"]);
        unmark(io, &x).unwrap();
        assert_eq!(board.named::<HITS>(), None);
        assert!(!claim(io, &x, b"unmarked").unwrap());
        assert!(mark(io, &y).unwrap());
        assert_eq!(board.named::<HITS>(), Some(second));
        assert!(hits(io, &y).unwrap().is_empty());
    }

    /// A mark over the name of a record freed while a holder has it open answers false, and succeeds once the holder
    /// closed it, after which the slots read empty.
    #[test]
    fn a_mark_takes_the_name_of_a_record_whose_slot_was_given_back() {
        let _process = crate::process();
        let io = &mut io();
        let ((first, x), (_, y)) = (held(io), held(io));
        assert!(mark(io, &x).unwrap());
        assert!(claim(io, &x, b"hit one!").unwrap());
        shared::free(shared::claim::<Test>(io, first).unwrap().expect("a held record is claimed"));
        assert!(!mark(io, &y).unwrap());
        assert!(claim(io, &x, b"hit two!").unwrap());
        drop(x);
        assert_eq!(super::state(first), shared::word::State::Free);
        assert!(mark(io, &y).unwrap());
        assert!(hits(io, &y).unwrap().is_empty());
    }

    /// A mark held right after its load of a stale name, while another record's mark takes the name and that
    /// record's claims are made, answers false and empties nothing: the winner's record reads both of its claims.
    #[test]
    fn a_mark_that_loses_the_name_empties_nothing() {
        let _process = crate::process();
        let io = &mut io();
        let ((gone, x), (first, a), (second, b)) = (held(io), held(io), held(io));
        assert!(mark(io, &x).unwrap());
        shared::free(shared::claim::<Test>(io, gone).unwrap().expect("a held record is claimed"));
        drop((x, a, b));
        let mut run = Run::new(1);
        let answers = Arc::new(Mutex::new(Vec::new()));
        for (name, location) in [("a", first), ("b", second)] {
            let answers = Arc::clone(&answers);
            run.actor(name, move |io| {
                let held = shared::open::<Test>(io, location)?.expect("a published record opens");
                let won = mark(io, &held)?;
                answers.lock().unwrap().push((name, won));
                if won {
                    assert!(claim(io, &held, b"hit one!")? && claim(io, &held, b"hit two!")?);
                }
                Ok(())
            });
        }
        run.until("b", c"doltgres-shared-marking");
        run.script(&[("a", u64::MAX), ("b", u64::MAX)]);
        assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
        assert_eq!(*answers.lock().unwrap(), [("a", true), ("b", false)]);
        let held = shared::open::<Test>(io, first).unwrap().expect("a published record opens");
        assert_eq!(hits(io, &held).unwrap(), [b"hit one!", b"hit two!"]);
    }

    /// A mark held right after its load of a live record's name, while that record is freed, sees the name's slot
    /// given back when it goes on, and takes the name.
    #[test]
    fn a_mark_held_after_its_load_sees_the_named_slot_given_back() {
        let _process = crate::process();
        let io = &mut io();
        let ((gone, x), (second, b)) = (held(io), held(io));
        assert!(mark(io, &x).unwrap());
        drop((x, b));
        let mut run = Run::new(1);
        let won = Arc::new(Mutex::new(None));
        let wins = Arc::clone(&won);
        run.actor("a", move |io| {
            shared::free(shared::claim::<Test>(io, gone)?.expect("a published record is claimed"));
            Ok(())
        });
        run.actor("b", move |io| {
            let held = shared::open::<Test>(io, second)?.expect("a published record opens");
            *wins.lock().unwrap() = Some(mark(io, &held)?);
            Ok(())
        });
        run.until("b", c"doltgres-shared-marking");
        run.script(&[("a", u64::MAX), ("b", u64::MAX)]);
        let report = run.finish();
        assert!(matches!(report.outcome, Outcome::Ended(_)), "{report:?}");
        assert_eq!(*won.lock().unwrap(), Some(true));
    }

    /// marked runs two actors that each mark a record of their own and, if their mark won, claim two slots through
    /// it, and returns which actors' marks won and the slots the winner's record reads.
    fn marked(seed: u64) -> (Vec<u8>, Vec<Vec<u8>>) {
        let io = &mut io();
        let mut run = Run::new(seed);
        let locations: Vec<u64> = (0..2).map(|_| shared::make::<Test>(io).unwrap().publish()).collect();
        let won = Arc::new(Mutex::new(Vec::new()));
        for (name, location) in [(b'a', locations[0]), (b'b', locations[1])] {
            let won = Arc::clone(&won);
            run.actor(&char::from(name).to_string(), move |io| {
                let held = shared::open::<Test>(io, location)?.expect("a published record opens");
                if mark(io, &held)? {
                    won.lock().unwrap().push(name);
                    assert!(claim(io, &held, &[name; 8])? && claim(io, &held, &[name; 8])?);
                }
                Ok(())
            });
        }
        assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
        let won = won.lock().unwrap().clone();
        let winner = locations[usize::from(won[0] - b'a')];
        let held = shared::open::<Test>(io, winner).unwrap().expect("a published record opens");
        (won, hits(io, &held).unwrap())
    }

    /// Of two marks that race for the name, one wins under every seed's interleaving, and the winner's record reads
    /// both of its claims, which the losing mark didn't empty.
    #[test]
    fn of_two_marks_one_wins_and_keeps_its_claims() {
        let _process = crate::process();
        for seed in 1..=24 {
            let (won, read) = marked(seed);
            assert_eq!(won.len(), 1, "seed {seed}: the marks that won were {won:?}");
            assert_eq!(read, [[won[0]; 8], [won[0]; 8]], "seed {seed}");
        }
    }

    /// A sweep lists a published record with its location and its atomic's value, whether or not a process holds it
    /// open, and not a record its maker hasn't published, a claimed one, a condemned one, a freed one, or another
    /// pool's.
    #[test]
    fn a_sweep_lists_the_published_records() {
        let _process = crate::process();
        let io = &mut io();
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
        let owned = shared::make::<Test>(io).unwrap();
        owned.record().atomic::<Count>(Op::Add(7));
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
        let location = owned.publish();
        shared::make::<Other>(io).unwrap().publish();
        assert_eq!(shared::sweep::<Test, Count>(io).unwrap(), [(location, 7)]);
        let held = shared::open::<Test>(io, location).unwrap().expect("a published record opens");
        assert_eq!(shared::sweep::<Test, Count>(io).unwrap(), [(location, 7)]);
        let claimed = shared::claim::<Test>(io, location).unwrap().expect("a held record is claimed");
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
        shared::free(claimed);
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
        drop(held);
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
    }

    /// A sweep held between its first load of a slot's word and its read of the record's atomic, while the record
    /// is claimed and freed and its slot is made again with another value and published, lists neither the old
    /// location nor the new value.
    #[test]
    fn a_sweep_lists_no_record_freed_and_made_again_under_it() {
        let _process = crate::process();
        let io = &mut io();
        let first = shared::make::<Test>(io).unwrap();
        first.record().atomic::<Count>(Op::Add(1));
        let location = first.publish();
        let rest: Vec<Owned<Test>> = (0..2).map(|_| shared::make(io).unwrap()).collect();
        let mut run = Run::new(1);
        let listed = Arc::new(Mutex::new(Vec::new()));
        let lists = Arc::clone(&listed);
        run.actor("s", move |io| {
            *lists.lock().unwrap() = shared::sweep::<Test, Count>(io)?;
            Ok(())
        });
        run.actor("w", move |io| {
            shared::free(shared::claim::<Test>(io, location)?.expect("a published record is claimed"));
            let again = shared::make::<Test>(io)?;
            again.record().atomic::<Count>(Op::Add(2));
            again.publish();
            Ok(())
        });
        run.script(&[("s", 2), ("w", u64::MAX), ("s", u64::MAX)]);
        assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
        assert_eq!(*listed.lock().unwrap(), []);
        assert_eq!(shared::sweep::<Test, Count>(io).unwrap().len(), 1);
        drop(rest);
    }

    /// swept runs a sweeper beside a writer that makes, publishes, and frees records, each with its own value, and
    /// returns the pairs the sweeps listed and the pairs the writer published.
    fn swept(seed: u64) -> [Vec<(u64, u64)>; 2] {
        let _io = io();
        let mut run = Run::new(seed);
        let (listed, published) = (Arc::new(Mutex::new(Vec::new())), Arc::new(Mutex::new(Vec::new())));
        let (lists, publishes) = (Arc::clone(&listed), Arc::clone(&published));
        run.actor("s", move |io| {
            for _ in 0..4 {
                lists.lock().unwrap().extend(shared::sweep::<Test, Count>(io)?);
            }
            Ok(())
        });
        run.actor("w", move |io| {
            for value in 1..=6 {
                let owned = shared::make::<Test>(io)?;
                owned.record().atomic::<Count>(Op::Add(value));
                let location = owned.publish();
                publishes.lock().unwrap().push((location, value));
                shared::free(shared::claim::<Test>(io, location)?.expect("a published record is claimed"));
            }
            Ok(())
        });
        assert!(matches!(run.finish().outcome, Outcome::Ended(_)));
        [std::mem::take(&mut *listed.lock().unwrap()), std::mem::take(&mut *published.lock().unwrap())]
    }

    /// Under every seed's interleaving of a sweep with makes, publishes, and frees, each pair a sweep lists is a
    /// location the writer published with the value that record held.
    #[test]
    fn a_sweep_lists_only_values_its_records_held() {
        let _process = crate::process();
        for seed in 1..=40 {
            let [listed, published] = swept(seed);
            assert!(listed.iter().all(|pair| published.contains(pair)), "seed {seed}: {listed:?} of {published:?}");
        }
    }

    /// The oldest epoch is a live reader's until it ends, and an advance returns the epoch before.
    #[test]
    fn the_oldest_epoch_is_a_live_readers() {
        let _process = crate::process();
        let (io, other) = (&mut io(), &mut Io::testing());
        let reader = read(io).unwrap();
        let mut held = epochs(other).unwrap();
        assert_eq!(held.advance(), reader.epoch());
        assert_eq!(held.oldest(), reader.epoch());
        drop(reader);
        assert_eq!(held.oldest(), held.advance());
    }

    /// A test begins with shared memory as the server starts it: a record the test before it left published, a
    /// word that test's process stored, and an entry it set are gone once its last hold of the process dropped.
    #[test]
    fn a_test_finds_nothing_the_test_before_it_left_in_shared_memory() {
        {
            let mut before = crate::io();
            let io = &mut *before;
            shared::make::<Test>(io).unwrap().publish();
            shared::mine::<Roots>(io).unwrap().store::<Root>(7);
            shared::single::<Board>().unwrap().lock::<Pin>(io).unwrap().set_entry::<NOTES>(2, b"note").unwrap();
        }
        let mut after = crate::io();
        let io = &mut *after;
        assert!(shared::sweep::<Test, Count>(io).unwrap().is_empty());
        assert_eq!(shared::load::<Roots, Root>(super::proc()).unwrap(), 0);
        assert_eq!(
            shared::single::<Board>().unwrap().lock_shared::<Pin>(io).unwrap().entry::<NOTES>(2).unwrap(),
            [0; 4]
        );
    }

    /// A location's record is gone once its slot was given back and never before: not while it is published, nor
    /// while a holder or a reclaim's look keeps a freed record condemned. Gone stays true when the slot is made
    /// again, and a location outside the pool is refused.
    #[test]
    fn a_record_is_gone_once_its_slot_is_given_back() {
        let _process = crate::process();
        let io = &mut io();
        let (location, held) = held(io);
        assert!(!shared::gone::<Test>(location).unwrap(), "a published record was gone");
        shared::free(shared::claim::<Test>(io, location).unwrap().expect("a held record is claimed"));
        assert!(!shared::gone::<Test>(location).unwrap(), "a condemned record was gone under its holder");
        drop(held);
        assert!(shared::gone::<Test>(location).unwrap());
        let unheld = shared::make::<Test>(io).unwrap().publish();
        shared::free(shared::claim::<Test>(io, unheld).unwrap().expect("a published record is claimed"));
        assert!(shared::gone::<Test>(unheld).unwrap());
        let looked = shared::make::<Test>(io).unwrap().publish();
        let Look::Mine(look) = shared::reclaim::<Test>(io, looked).unwrap() else { panic!("the reclaim takes it") };
        shared::free(shared::claim::<Test>(io, looked).unwrap().expect("a looked-at record is claimed"));
        assert!(!shared::gone::<Test>(looked).unwrap(), "a condemned record was gone under a reclaim's look");
        drop(look);
        assert!(shared::gone::<Test>(looked).unwrap());
        let again: Vec<u64> = (0..3).map(|_| shared::make::<Test>(io).unwrap().publish()).collect();
        assert!(again.iter().any(|made| shared::word::slot(*made) == shared::word::slot(location)));
        assert!(shared::gone::<Test>(location).unwrap(), "a record came back when its slot was made again");
        assert!(again.iter().all(|made| !shared::gone::<Test>(*made).unwrap()));
        assert_eq!(sqlstate(shared::gone::<Other>(location)), Some(*b"XX000"));
        assert_eq!(sqlstate(shared::gone::<Test>(99 << 32)), Some(*b"XX000"));
    }

    /// AT is the location of the record the tests' exit works reach.
    static AT: AtomicU64 = AtomicU64::new(0);

    /// SEEN is what `Sees` saw at each of its runs: its process, its process's second word, and whether the record
    /// at `AT` was gone.
    static SEEN: Mutex<Vec<(u32, u64, bool)>> = Mutex::new(Vec::new());

    /// Sees is an exit work that notes what its process still has as the work runs.
    struct Sees;
    impl host::Exit for Sees {
        fn run(io: &mut Io) -> Result<(), host::Error> {
            let word = shared::mine::<Roots>(io)?.load::<Tail>();
            let gone = shared::gone::<Test>(AT.load(Ordering::Relaxed))?;
            SEEN.lock().unwrap().push((shared::proc_number(), word, gone));
            Ok(())
        }
    }

    /// keeps registers `Sees` three times, stores the process's second word, and opens the record at `AT`.
    fn keeps(io: &mut Io) -> Result<Held<Test>, host::Error> {
        for _ in 0..3 {
            host::at_exit::<Sees>()?;
        }
        shared::mine::<Roots>(io)?.store::<Tail>(41);
        let held = shared::open::<Test>(io, AT.load(Ordering::Relaxed))?;
        held.ok_or_else(|| host::Error::fork(b"XX000", "the record didn't open".to_owned()))
    }

    /// An actor's exit work runs once however often it was registered, when the actor returns and when it is
    /// terminated, while the actor still has its words and its handles: its word isn't zeroed yet, and a record a
    /// terminated actor's abandoned frame holds, freed meanwhile, isn't gone. After the run the words are zero and
    /// the record is gone.
    #[test]
    fn an_actors_exit_work_runs_before_its_exit_gives_anything_up() {
        let _process = crate::process();
        let io = &mut io();
        let location = shared::make::<Test>(io).unwrap().publish();
        AT.store(location, Ordering::Relaxed);
        SEEN.lock().unwrap().clear();
        let mut run = Run::new(5);
        run.actor("returns", |io| {
            let _held = keeps(io)?;
            let owned = shared::claim::<Test>(io, AT.load(Ordering::Relaxed))?;
            shared::free(owned.ok_or_else(|| host::Error::fork(b"XX000", "the record wasn't claimed".to_owned()))?);
            Ok(())
        });
        run.actor("terminated", |io| {
            let _held = keeps(io)?;
            let lone = shared::single::<Lone>()?;
            loop {
                lone.atomic::<Total>(Op::Load);
            }
        });
        run.script(&[("terminated", 12), ("returns", u64::MAX)]);
        run.terminate_at("terminated", 40);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Returned(Ok(())));
        assert_eq!(endings[1].1, Ending::Terminated);
        assert!(report.exits.is_empty(), "{report:?}");
        let seen = std::mem::take(&mut *SEEN.lock().unwrap());
        assert_eq!(seen.iter().map(|&(_, word, gone)| (word, gone)).collect::<Vec<_>>(), [(41, false); 2]);
        assert_ne!(seen[0].0, seen[1].0);
        for (proc, ..) in seen {
            assert_eq!(shared::load::<Roots, Tail>(proc).unwrap(), 0);
        }
        assert!(shared::gone::<Test>(location).unwrap(), "an exited actor's handle kept a freed record");
    }

    /// RAN counts the runs of `Counts`, and REFUSED holds the SQLSTATEs of the registrations `Fails` made.
    static RAN: AtomicU64 = AtomicU64::new(0);
    static REFUSED: Mutex<Vec<Option<[u8; 5]>>> = Mutex::new(Vec::new());

    /// Counts is an exit work that counts its runs.
    struct Counts;
    impl host::Exit for Counts {
        fn run(_: &mut Io) -> Result<(), host::Error> {
            RAN.fetch_add(1, Ordering::Relaxed);
            Ok(())
        }
    }

    /// Fails is an exit work that registers a new work and one already listed, and then fails.
    struct Fails;
    impl host::Exit for Fails {
        fn run(_: &mut Io) -> Result<(), host::Error> {
            REFUSED.lock().unwrap().extend([sqlstate(host::at_exit::<Sees>()), sqlstate(host::at_exit::<Counts>())]);
            Err(host::Error::fork(b"XX000", "the exit work failed".to_owned()))
        }
    }

    /// A work that fails doesn't stop the work registered before it, and its error is in the run's report; a
    /// registration made once the exit works have begun is refused, of a new work and of one already listed.
    #[test]
    fn a_failed_exit_work_stops_no_other() {
        let _process = crate::process();
        let _io = io();
        RAN.store(0, Ordering::Relaxed);
        REFUSED.lock().unwrap().clear();
        let mut run = Run::new(2);
        run.actor("a", |_| {
            host::at_exit::<Counts>()?;
            host::at_exit::<Fails>()
        });
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Returned(Ok(())));
        assert_eq!(report.exits, [("a".to_owned(), *b"XX000", "the exit work failed".to_owned())]);
        assert_eq!(RAN.load(Ordering::Relaxed), 1);
        assert_eq!(*REFUSED.lock().unwrap(), [Some(*b"XX000"); 2]);
    }

    /// STAGE is how far `Pauses` got: 1 before its pause, 2 after it.
    static STAGE: AtomicU64 = AtomicU64::new(0);

    /// Pauses is an exit work with a named pause in it.
    struct Pauses;
    impl host::Exit for Pauses {
        fn run(io: &mut Io) -> Result<(), host::Error> {
            STAGE.store(1, Ordering::Relaxed);
            host::pause!(io, c"doltgres-sim-exiting")?;
            STAGE.store(2, Ordering::Relaxed);
            Ok(())
        }
    }

    /// A script runs a terminated actor to a pause inside its exit work and holds it there while another actor
    /// runs, and the work ends when the script lets it.
    #[test]
    fn a_terminated_actor_is_held_inside_its_exit_work() {
        let _process = crate::process();
        let _io = io();
        STAGE.store(0, Ordering::Relaxed);
        let seen = Arc::new(AtomicU64::new(0));
        let sees = Arc::clone(&seen);
        let mut run = Run::new(4);
        run.actor("a", |_| {
            host::at_exit::<Pauses>()?;
            let lone = shared::single::<Lone>()?;
            loop {
                lone.atomic::<Total>(Op::Load);
            }
        });
        run.actor("b", move |_| {
            sees.store(STAGE.load(Ordering::Relaxed), Ordering::Relaxed);
            Ok(())
        });
        run.terminate_at("a", 3);
        run.until("a", c"doltgres-sim-exiting");
        run.script(&[("b", u64::MAX), ("a", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!((seen.load(Ordering::Relaxed), STAGE.load(Ordering::Relaxed)), (1, 2));
    }

    /// TOOK counts the runs of `Takes` that took their flag.
    static TOOK: AtomicU64 = AtomicU64::new(0);

    /// Takes is an exit work that takes the first flag of the record at `AT`.
    struct Takes;
    impl host::Exit for Takes {
        fn run(io: &mut Io) -> Result<(), host::Error> {
            let held = shared::open::<Test>(io, AT.load(Ordering::Relaxed))?;
            let held = held.ok_or_else(|| host::Error::fork(b"XX000", "the record didn't open".to_owned()))?;
            drop(held.record().flag::<First>(io)?);
            TOOK.fetch_add(1, Ordering::Relaxed);
            Ok(())
        }
    }

    /// An actor terminated while it holds a flag has the flag cleared before its exit work runs, so the work, which
    /// takes a flag another actor holds while that actor waits for the terminated one's, doesn't deadlock with it.
    #[test]
    fn a_terminated_actors_flags_are_cleared_before_its_exit_work() {
        let _process = crate::process();
        let io = &mut io();
        let (kept, wanted) = (shared::make::<Test>(io).unwrap().publish(), shared::make::<Test>(io).unwrap().publish());
        AT.store(wanted, Ordering::Relaxed);
        TOOK.store(0, Ordering::Relaxed);
        let mut run = Run::new(6);
        run.actor("a", move |io| {
            host::at_exit::<Takes>()?;
            let held = shared::open::<Test>(io, kept)?.expect("a published record opens");
            let record = held.record();
            let mut flag = record.flag::<First>(io)?;
            host::pause!(&mut flag.io(), c"doltgres-sim-holds-its-flag")?;
            loop {
                record.atomic::<Count>(Op::Load);
            }
        });
        run.actor("b", move |io| {
            let held = shared::open::<Test>(io, wanted)?.expect("a published record opens");
            let record = held.record();
            let _flag = record.flag::<First>(io)?;
            let other = &mut Io::testing();
            let first = shared::open::<Test>(other, kept)?.expect("a published record opens");
            drop(first.record().flag::<First>(other)?);
            Ok(())
        });
        run.terminate_at("a", 1000);
        run.until("a", c"doltgres-sim-holds-its-flag");
        run.script(&[("b", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!(endings[1].1, Ending::Returned(Ok(())));
        assert!(report.exits.is_empty(), "{report:?}");
        assert_eq!(TOOK.load(Ordering::Relaxed), 1);
    }
}
