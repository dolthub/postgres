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

//! Shared memory (design item 14's S2): records a storage crate declares by a shape of typed parts, reached only
//! through `host`'s doors by part, so no storage crate sees an address. All of it is fixed memory, sized when the
//! server starts: a kind is a pool of a shape's records, a single record, or words for each process, listed once in
//! the server's table of kinds. A record's locks are taken only in the order its shape declares, a blob and an
//! array's entry are written only under their own lock held exclusively, a single record's slots are claimed only
//! under the barrier of the pooled record they name, and a lock held across I/O lends a capability that never waits,
//! all by the parts' types: a
//! record's locks and flags, and the epochs, are taken from the backend's `Io` itself, which every guard has
//! borrowed. A pooled record is reached only through a handle that keeps its slot: its owner's, who is its maker
//! until it publishes it and then the one process that claims it, a holder's, or a reclaim's.

use std::marker::PhantomData;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::io::sealed::{self, Sealed as _};
use crate::io::{Held as Locks, Io, IoDoors, ReadDoors, Registering, TakesShort};
use crate::{Error, Host, MayRaise, current, doors};

pub mod word;

/// Marks is what a guard or handle names by type alone: its shape and parts, which it holds no value of.
type Marks<T> = PhantomData<fn() -> T>;

/// Shape is a record's parts: its id, unique among the fork's shapes, and its counts of each kind of part.
pub trait Shape: 'static {
    /// ID tells this shape's records from every other shape's.
    const ID: u64;
    /// LOCKS is the number of the record's locks, short and long.
    const LOCKS: usize;
    /// FLAGS is the number of the record's flags.
    const FLAGS: usize;
    /// ATOMICS is the number of the record's atomics.
    const ATOMICS: usize;
    /// BLOBS gives each of the record's blobs its largest size in bytes.
    const BLOBS: &'static [usize];
    /// ARRAYS gives each of the record's arrays its count of entries and an entry's size in bytes.
    const ARRAYS: &'static [(usize, usize)];
    /// LISTS is the number of the record's lists, each nine words past its atomics (`delta::list`).
    const LISTS: usize = 0;
}

/// ListAt is one of a shape's lists: the lock under which it is loaded and, held exclusively, stored.
pub trait ListAt<const I: usize>: Shape + Sized {
    /// Lock is the record's lock that guards the list.
    type Lock: Lock<Self>;
}

/// Removes is a list that takes `insert` and `remove`, whose entries may move between indexes.
pub trait Removes<const I: usize>: ListAt<I> {}

/// Limits are the numbers Postgres fixes before the fork's shared memory is sized, which a pool's count reads.
pub struct Limits {
    /// processes is the most processes the server runs at once: its backends and its auxiliary processes.
    pub processes: usize,
    /// delta_pages is the number of slots of the delta's page directory, one for each 1 KiB of `delta_memory`.
    pub delta_pages: usize,
    /// relation_locks is how many relation locks Postgres can grant at once: its shared lock table's entries, copied
    /// from lock.c's `NLOCKENTS`, which lock.c keeps to itself, and every backend's fast-path slots, their groups
    /// copied from postinit.c's `InitializeFastPathLocks`, since a child sizes the fork's memory before Postgres
    /// computes them.
    pub relation_locks: usize,
}

/// Pool is a shape the server keeps a pool of records of.
pub trait Pool: Shape {
    /// NAME names the pool's records in the error of a pool with every record in use.
    const NAME: &'static str;

    /// count returns how many records the pool holds.
    fn count(limits: &Limits) -> usize;

    /// reclaim frees what records of the pool it can, for a make that found no free slot, and reports whether the
    /// make should look again: it freed a record, or met one it couldn't decide (one another reclaim was looking at,
    /// or one a process opened while it looked). It answers false once it looked at every record and each was in use.
    fn reclaim(io: &mut Io) -> Result<bool, Error>;
}

/// Aged is a shape the server keeps a pool of records of that the delta's epochs free (design item 14's S6): a
/// record is made, linked by the store that names it, retired, and freed once every hold that began before its
/// retirement has ended, with no holders and no reclaim.
pub trait Aged: Shape {
    /// NAME names the pool's records in the error of a pool whose every slot is spent.
    const NAME: &'static str;
    /// NAMES are the ids of the aged shapes whose records this shape's records name, which the server's table lists
    /// after this one, since the free pass frees the kinds in the table's order.
    const NAMES: &'static [u64];

    /// count returns how many records the pool holds.
    fn count(limits: &Limits) -> usize;
}

/// Single is a shape the server keeps one record of, made when the server starts and never freed.
pub trait Single: Shape {
    /// words returns the count of the record's run of words past its atomics, which follows the server's limits;
    /// a shape that declares no run has none.
    fn words(_: &Limits) -> usize {
        0
    }
}

/// ProcSlots is a slot for each process: a count of words, which only the slot's process stores and any process
/// loads, zero from the process's start and again from its exit, unless the kind is kept.
pub trait ProcSlots: 'static {
    /// ID tells these slots from every shape and every other slots.
    const ID: u64;
    /// WORDS is the number of a slot's words.
    const WORDS: usize;
    /// KEPT says whether the words are left as they are at their process's exit, for their crate to clear through the
    /// number's next owner.
    const KEPT: bool = false;
}

/// Word is one of a process slot's words.
pub trait Word<P: ProcSlots>: 'static {
    /// INDEX is the word's place among the slot's words, in which an exit zeroes them.
    const INDEX: usize;
}

/// BLOBS is the most blobs a shape declares.
pub const BLOBS: usize = 4;

/// ARRAYS is the most arrays a shape declares.
pub const ARRAYS: usize = 4;

/// Form is how the server keeps a kind.
pub enum Form {
    /// Pool is a pool of records, with the function that returns its count.
    Pool(fn(&Limits) -> usize),
    /// Single is one record, with the function that returns the count of its run of words.
    Single { run: fn(&Limits) -> usize },
    /// Aged is a pool of records the delta's epochs free, with the function that returns its count and the ids of
    /// the aged shapes its records name.
    Aged { count: fn(&Limits) -> usize, names: &'static [u64] },
    /// Process is a slot of words for each process, with whether an exit leaves the words as they are.
    Process { kept: bool },
}

/// Kind is a line of the server's table of kinds: a shape and how the server keeps it.
pub struct Kind {
    /// id is the shape's id.
    pub id: u64,
    /// locks is the number of locks.
    pub locks: usize,
    /// flags is the number of flags.
    pub flags: usize,
    /// atomics is the number of atomics, and of a process slot's words.
    pub atomics: usize,
    /// blobs gives each blob's largest size.
    pub blobs: &'static [usize],
    /// arrays gives each array's count of entries and an entry's size.
    pub arrays: &'static [(usize, usize)],
    /// form is how the server keeps the kind.
    pub form: Form,
}

impl Kind {
    /// of returns a shape's line in a form.
    const fn of<S: Shape>(form: Form) -> Kind {
        assert!(S::BLOBS.len() <= BLOBS, "a shape declares more blobs than a record holds");
        assert!(S::ARRAYS.len() <= ARRAYS, "a shape declares more arrays than a record holds");
        Kind {
            id: S::ID,
            locks: S::LOCKS,
            flags: S::FLAGS,
            atomics: S::ATOMICS + crate::delta::list::WORDS * S::LISTS,
            blobs: S::BLOBS,
            arrays: S::ARRAYS,
            form,
        }
    }

    /// pool returns the line of a pool.
    pub const fn pool<S: Pool>() -> Kind {
        Kind::of::<S>(Form::Pool(S::count))
    }

    /// aged returns the line of a pool the delta's epochs free, whose records hold one atomic past their shape's,
    /// the host's, for the epoch of their retirement.
    pub const fn aged<S: Aged>() -> Kind {
        let mut kind = Kind::of::<S>(Form::Aged { count: S::count, names: S::NAMES });
        kind.atomics += 1;
        kind
    }

    /// single returns the line of a single record.
    pub const fn single<S: Single>() -> Kind {
        Kind::of::<S>(Form::Single { run: S::words })
    }

    /// slots returns the line of a slot for each process.
    pub const fn slots<P: ProcSlots>() -> Kind {
        Kind {
            id: P::ID,
            locks: 0,
            flags: 0,
            atomics: P::WORDS,
            blobs: &[],
            arrays: &[],
            form: Form::Process { kept: P::KEPT },
        }
    }
}

/// ordered reports whether a table lists every aged kind before each aged kind it names and names only aged kinds
/// the table lists, so a free pass that walks the kinds in the table's order frees a record no later than what it
/// names.
pub const fn ordered(kinds: &[Kind]) -> bool {
    let mut at = 0;
    while at < kinds.len() {
        if let Form::Aged { names, .. } = &kinds[at].form {
            let mut named = 0;
            while named < names.len() {
                let mut other = 0;
                while other < kinds.len() && kinds[other].id != names[named] {
                    other += 1;
                }
                if other == kinds.len() || other <= at || !matches!(kinds[other].form, Form::Aged { .. }) {
                    return false;
                }
                named += 1;
            }
        }
        at += 1;
    }
    true
}

/// Lock is a record's lock under which no file I/O runs.
pub trait Lock<S: Shape>: 'static {
    /// INDEX is the lock's place among the record's locks.
    const INDEX: usize;
}

/// LongLock is a record's lock held across file I/O.
pub trait LongLock<S: Shape>: 'static {
    /// INDEX is the lock's place among the record's locks.
    const INDEX: usize;
}

/// Flag is a record's flag, held across a long operation, which other processes wait on.
pub trait Flag<S: Shape>: 'static {
    /// INDEX is the flag's place among the record's flags.
    const INDEX: usize;
}

/// Atomic is one of a record's atomic numbers.
pub trait Atomic<S: Shape>: 'static {
    /// INDEX is the atomic's place among the record's atomics.
    const INDEX: usize;
}

/// BlobAt declares, once for each index, a blob of a shape: bytes at most the size the shape's `BLOBS` gives the
/// index, read and written whole under the one lock `Guard` that guards them. A shape has one declaration of an
/// index, so no second one can name another lock.
pub trait BlobAt<const I: usize>: Shape {
    /// Guard is the lock that guards the blob.
    type Guard: 'static;
}

/// ArrayAt declares, once for each index, an array of a shape: its discipline, with what guards it, and its entries'
/// bytes. A shape has one declaration of an index, so no second one can name another guard or another discipline.
pub trait ArrayAt<const I: usize>: Shape {
    /// By is the array's discipline: `Entries` or `Slots`.
    type By: 'static;
    /// Entry is an entry's bytes.
    type Entry: Bytes;
}

/// Bytes is an entry's bytes: an array of as many bytes as the entry's size.
pub trait Bytes: bytes::Sealed + 'static {
    /// bytes returns the entry's bytes.
    fn bytes(&self) -> &[u8];
}

/// bytes keeps `Bytes` to arrays of bytes.
mod bytes {
    /// Sealed is implemented only by arrays of bytes.
    pub trait Sealed {}
}

impl<const N: usize> bytes::Sealed for [u8; N] {}

impl<const N: usize> Bytes for [u8; N] {
    fn bytes(&self) -> &[u8] {
        self
    }
}

/// Entries is the discipline of an array whose entries are read and written one at a time under the lock `L` of the
/// array's own record.
pub struct Entries<L>(Marks<L>);

/// Slots is the discipline of a single record's array whose slots processes claim one at a time under the long lock
/// `B` of the one record of the pooled shape `Of` that the array names, held shared, and whose claimed slots the
/// holder of that barrier, exclusively, reads.
pub struct Slots<Of, B>(Marks<(Of, B)>);

/// Barred is a slots array's discipline, which gives the pooled shape and the long lock that guard the array.
pub trait Barred {
    /// Of is the pooled shape whose records' barrier guards the array.
    type Of: Pool;
    /// Barrier is the long lock of `Of` that guards the array.
    type Barrier: LongLock<Self::Of>;
}

impl<Of: Pool, B: LongLock<Of>> Barred for Slots<Of, B> {
    type Of = Of;
    type Barrier = B;
}

/// Of is the pooled shape whose records' barrier guards a shape's slots array at an index.
type Of<S, const I: usize> = <<S as ArrayAt<I>>::By as Barred>::Of;

/// Barrier is the long lock that guards a shape's slots array at an index.
type Barrier<S, const I: usize> = <<S as ArrayAt<I>>::By as Barred>::Barrier;

/// array returns the line of a shape's `ARRAYS` for its array at an index, with a count of entries and the size of
/// the entry the array's declaration gives.
pub const fn array<S: ArrayAt<I>, const I: usize>(count: usize) -> (usize, usize) {
    (count, size_of::<S::Entry>())
}

/// After says that `Self`, a lock or flag, may be taken while `L` is held.
pub trait After<L> {}

/// Exclusive is a lock held by its taker alone, under which the blobs it guards may be written.
pub struct Exclusive;

/// Shared is a lock its taker shares with other readers, under which the blobs it guards are only read.
pub struct Shared;

/// distinct reports whether indices are different from one another and below a count, which a shape's crate asserts
/// of its markers at compile time.
pub const fn distinct(indices: &[usize], count: usize) -> bool {
    let mut i = 0;
    while i < indices.len() {
        if indices[i] >= count {
            return false;
        }
        let mut j = i + 1;
        while j < indices.len() {
            if indices[i] == indices[j] {
                return false;
            }
            j += 1;
        }
        i += 1;
    }
    true
}

/// Op is an operation on an atomic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Op {
    /// Load reads the value.
    Load,
    /// Add adds to the value, returning the value before.
    Add(u64),
    /// Exchange writes the second value if the first is the value, returning the value before.
    Exchange(u64, u64),
}

/// Owned is a pooled record one process owns, its maker or the one claimant after it was published: only it frees
/// the record. One dropped without a free or a publish gives the record up at once: a record never published is
/// freed, and one claimed is published again.
pub struct Owned<S> {
    /// host is the host that made or claimed the record, which gives it up when the handle drops.
    host: &'static dyn Host,
    place: u64,
    shape: Marks<S>,
}

/// make makes a record of a pool, its atomics zero, its blobs empty, and its flags clear, owned by its maker. When
/// every slot is taken it has the pool reclaim what it can and looks again, for as long as the reclaim says to, and
/// fails only once every record of the pool is in use.
pub fn make<S: Pool>(io: &mut Io) -> Result<Owned<S>, Error> {
    let host = current();
    loop {
        if let Some(place) = host.shared_make(&doors(io), &Kind::pool::<S>())? {
            return Ok(Owned { host, place, shape: PhantomData });
        }
        if !S::reclaim(io)? {
            return match host.shared_make(&doors(io), &Kind::pool::<S>())? {
                Some(place) => Ok(Owned { host, place, shape: PhantomData }),
                None => Err(Error::fork(b"XX000", format!("every {} record is in use", S::NAME))),
            };
        }
        host.check_interrupts(&doors(io))?;
    }
}

/// claim takes the ownership of a published record at a location, whatever processes hold it, after which no process
/// opens it. It answers nothing for a location that is stale or a record that isn't published. Its caller shows the
/// entry's `Io` or the guard it read the location under, of which the claim reads nothing.
pub fn claim<S: Pool>(_: &mut impl Within, location: u64) -> Result<Option<Owned<S>>, Error> {
    let host = current();
    let place = host.shared_claim(&Kind::pool::<S>(), location)?;
    Ok(place.map(|place| Owned { host, place, shape: PhantomData }))
}

/// free frees a record: at once when no process holds it, and otherwise when the last that does closes it.
pub fn free<S: Pool>(owned: Owned<S>) {
    owned.host.shared_free(owned.place);
}

impl<S: Pool> Owned<S> {
    /// record returns the record, reached for as long as its owner is.
    pub fn record(&self) -> Record<'_, S> {
        Record { place: self.place, held: PhantomData, shape: PhantomData }
    }

    /// publish gives up the ownership, so any process may open the record and one may claim it, and returns its
    /// location for the owner to publish.
    pub fn publish(self) -> u64 {
        self.host.shared_publish(self.place)
    }
}

impl<S> Drop for Owned<S> {
    fn drop(&mut self) {
        self.host.shared_give_up(self.place);
    }
}

/// Held is a published record a process opened, which keeps its slot until the handle drops.
pub struct Held<S> {
    /// host is the host that opened the record, which closes it when the handle drops.
    host: &'static dyn Host,
    place: u64,
    shape: Marks<S>,
}

/// Within is what the doors that take a location take: an entry's `Io`, or a guard of a record's lock, since a crate
/// opens, claims, and reclaims a record under the lock it read the record's location under.
pub trait Within: within::Raises {}

/// within keeps `Within` to this module's views of an entry.
mod within {
    /// Raises is implemented only by this module's views of an entry.
    pub trait Raises {}
}

impl within::Raises for Io {}

impl Within for Io {}

impl within::Raises for Registering<'_> {}

impl<S, L, M> within::Raises for Guard<'_, S, L, M> {}

impl<S, L, M> Within for Guard<'_, S, L, M> {}

impl<S, L, M> within::Raises for LongGuard<'_, S, L, M> {}

impl<S, L, M> Within for LongGuard<'_, S, L, M> {}

impl<S, L> within::Raises for LongIo<'_, S, L> {}

impl<S, F> within::Raises for FlagIo<'_, S, F> {}

/// Pauses is what a crate holds at a place where the fork's tests can hold it (`pause!`): an entry's `Io`, a
/// registration's view of it, a lender's capability, or a guard of a record's lock.
pub trait Pauses: within::Raises {}

impl Pauses for Io {}

impl Pauses for Registering<'_> {}

impl<S, L> Pauses for LongIo<'_, S, L> {}

impl<S, F> Pauses for FlagIo<'_, S, F> {}

impl<S, L, M> Pauses for Guard<'_, S, L, M> {}

impl<S, L, M> Pauses for LongGuard<'_, S, L, M> {}

/// open opens a published record at a location. It answers nothing for a location that is stale or a record that
/// isn't published, which the crate that keeps the location says the meaning of.
pub fn open<S: Pool>(_: &mut impl Within, location: u64) -> Result<Option<Held<S>>, Error> {
    let host = current();
    let place = host.shared_open(&MayRaise::of(), &Kind::pool::<S>(), location)?;
    Ok(place.map(|place| Held { host, place, shape: PhantomData }))
}

impl<S: Pool> Held<S> {
    /// record returns the record, reached for as long as the handle lives.
    pub fn record(&self) -> Record<'_, S> {
        Record { place: self.place, held: PhantomData, shape: PhantomData }
    }

    /// place returns the record's place, for a caller that adds to its atomics while the handle lives where it may
    /// take no lock and get no result, a commit's critical section.
    pub fn place(&self) -> HeldPlace<S> {
        HeldPlace { place: self.place, shape: PhantomData }
    }
}

/// HeldPlace is the place of a record a hold keeps open, which `Held::place` gives out and its caller uses only while
/// that hold lives: its adds take no lock, allocate nothing, and return nothing.
pub struct HeldPlace<S> {
    place: u64,
    shape: Marks<S>,
}

impl<S> Clone for HeldPlace<S> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<S> Copy for HeldPlace<S> {}

impl<S: Shape> HeldPlace<S> {
    /// add adds to one of the record's atomics, wrapping, as `Record::atomic` with `Op::Add` does.
    pub fn add<A: Atomic<S>>(self, value: u64) {
        current().atomic(self.place, A::INDEX, Op::Add(value));
    }

    /// exchange writes one of the record's atomics if it holds a value, as `Record::atomic` with `Op::Exchange` does,
    /// returning its value before.
    pub fn exchange<A: Atomic<S>>(self, old: u64, new: u64) -> u64 {
        current().atomic(self.place, A::INDEX, Op::Exchange(old, new))
    }
}

/// PlaceCell holds a `HeldPlace`, or none, which its process reads with no lock and no result, in a commit's critical
/// section or as it exits, and uses only while the hold the place came from lives: one past the place, zero for none.
pub struct PlaceCell<S> {
    place: AtomicU64,
    shape: Marks<S>,
}

impl<S> Default for PlaceCell<S> {
    fn default() -> Self {
        PlaceCell { place: AtomicU64::new(0), shape: PhantomData }
    }
}

impl<S> PlaceCell<S> {
    /// set holds a place, or none.
    pub fn set(&self, place: Option<HeldPlace<S>>) {
        self.place.store(place.map_or(0, |place| place.place.wrapping_add(1)), Ordering::Relaxed);
    }

    /// get returns the place the cell holds, if any.
    pub fn get(&self) -> Option<HeldPlace<S>> {
        match self.place.load(Ordering::Relaxed) {
            0 => None,
            stored => Some(HeldPlace { place: stored.wrapping_sub(1), shape: PhantomData }),
        }
    }
}

impl<S> Drop for Held<S> {
    fn drop(&mut self) {
        self.host.shared_close(self.place);
    }
}

/// Reclaim is a published record no process held when a reclaim took it to look at, which it frees only if no
/// process opened the record since.
pub struct Reclaim<S> {
    /// host is the host that took the record, which gives it up when the handle drops.
    host: &'static dyn Host,
    place: u64,
    shape: Marks<S>,
}

/// Look is what a reclaim's look at a record finds.
pub enum Look<S> {
    /// Mine is the record, taken.
    Mine(Reclaim<S>),
    /// Theirs is a record another reclaim is looking at.
    Theirs,
    /// Used is a record that is held, claimed, or being freed, or a location that is stale.
    Used,
}

/// Looked is what a host's look at a record finds.
pub enum Looked {
    /// Mine is the place of the record, taken.
    Mine(u64),
    /// Theirs is a record another reclaim is looking at.
    Theirs,
    /// Used is a record that is held, claimed, or being freed, or a location that is stale.
    Used,
}

/// reclaim looks at a published record at a location for a pool's reclaim, taking it when no process holds it and no
/// other reclaim is looking at it. Its caller shows the entry's `Io` or the guard it read the location under, of
/// which the reclaim reads nothing.
pub fn reclaim<S: Pool>(_: &mut impl Within, location: u64) -> Result<Look<S>, Error> {
    let host = current();
    Ok(match host.shared_reclaim(&Kind::pool::<S>(), location)? {
        Looked::Mine(place) => Look::Mine(Reclaim { host, place, shape: PhantomData }),
        Looked::Theirs => Look::Theirs,
        Looked::Used => Look::Used,
    })
}

impl<S: Pool> Reclaim<S> {
    /// record returns the record, reached for as long as the reclaim looks at it.
    pub fn record(&self) -> Record<'_, S> {
        Record { place: self.place, held: PhantomData, shape: PhantomData }
    }

    /// free frees the record if no process opened it since the reclaim took it, and otherwise gives it up, reporting
    /// whether it freed it.
    pub fn free(self) -> bool {
        self.host.shared_reclaimed(self.place)
    }
}

impl<S> Drop for Reclaim<S> {
    fn drop(&mut self) {
        self.host.shared_unreclaim(self.place);
    }
}

/// sweep lists the published records of a pool: for each, its location and the value of its atomic `A`, read with
/// no handle. A record is listed only if its slot held it, published, both before and after the read, so the value
/// is that record's. The list is a hint: a record in it may be claimed or freed, and its slot made again, as soon
/// as the sweep has passed it, so a caller acts on a pair only by opening, claiming, or reclaiming its location, and
/// a record published after the sweep passed its slot isn't in it.
pub fn sweep<S: Pool, A: Atomic<S>>(_: &mut impl Within) -> Result<Vec<(u64, u64)>, Error> {
    current().shared_sweep(&MayRaise::of(), &Kind::pool::<S>(), A::INDEX)
}

/// gone reports whether the record a location named no longer has its slot: the slot's generation has moved on, or
/// the slot is spent. It takes no handle, any process may ask at any time, true is final, and a location outside
/// the shape's pool is refused.
pub fn gone<S: Pool>(location: u64) -> Result<bool, Error> {
    current().shared_gone(&Kind::pool::<S>(), location)
}

/// single returns the server's one record of a shape.
pub fn single<S: Single>() -> Result<Record<'static, S>, Error> {
    Ok(Record { place: current().shared_single(&Kind::single::<S>())?, held: PhantomData, shape: PhantomData })
}

/// Record is a record of a shape reached while the handle it came from lives: its place, which every process reads
/// the same way.
pub struct Record<'r, S> {
    place: u64,
    held: PhantomData<&'r ()>,
    shape: Marks<S>,
}

impl<S> Clone for Record<'_, S> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<S> Copy for Record<'_, S> {}

impl<S> Record<'_, S> {
    /// at returns the record at a place, which its caller reached it by.
    pub(crate) fn at(place: u64) -> Self {
        Record { place, held: PhantomData, shape: PhantomData }
    }
}

impl<'r, S: Shape> Record<'r, S> {
    /// lock takes a lock under which no file I/O runs, exclusively.
    pub fn lock<'a, L: Lock<S>>(&'a self, io: &'a mut impl TakesShort) -> Result<Guard<'a, S, L, Exclusive>, Error> {
        let held = io.parts();
        take(current(), self.place, L::INDEX, true)?;
        Ok(Guard::new(self.place, L::INDEX, held))
    }

    /// lock_shared takes a lock under which no file I/O runs, shared.
    pub fn lock_shared<'a, L: Lock<S>>(
        &'a self,
        io: &'a mut impl TakesShort,
    ) -> Result<Guard<'a, S, L, Shared>, Error> {
        let held = io.parts();
        take(current(), self.place, L::INDEX, false)?;
        Ok(Guard::new(self.place, L::INDEX, held))
    }

    /// lock_long takes a lock held across file I/O, exclusively.
    pub fn lock_long<'a, L: LongLock<S>>(&'a self, io: &'a mut impl TakesShort) -> Result<LongGuard<'a, S, L>, Error> {
        let held = io.parts();
        take(current(), self.place, L::INDEX, true)?;
        Ok(LongGuard::new(self.place, L::INDEX, held))
    }

    /// lock_long_shared takes a lock held across file I/O, shared.
    pub fn lock_long_shared<'a, L: LongLock<S>>(
        &'a self,
        io: &'a mut Io,
    ) -> Result<LongGuard<'a, S, L, Shared>, Error> {
        let held = io.parts();
        take(current(), self.place, L::INDEX, false)?;
        Ok(LongGuard::new(self.place, L::INDEX, held))
    }

    /// flag sets a flag, waiting while another process holds it, and refusing one its own process holds.
    pub fn flag<'a, F: Flag<S>>(&'a self, io: &'a mut impl TakesShort) -> Result<FlagGuard<'a, S, F>, Error> {
        let held = io.parts();
        current().flag_take(&MayRaise::of(), self.place, F::INDEX)?;
        Ok(FlagGuard { host: current(), place: self.place, index: F::INDEX, held, flag: PhantomData })
    }

    /// atomic operates on an atomic, returning its value before.
    pub fn atomic<A: Atomic<S>>(self, op: Op) -> u64 {
        current().atomic(self.place, A::INDEX, op)
    }

    /// names returns one of the record's atomics as a word that takes a page's name at the page's link.
    pub fn names<A: Atomic<S>>(self) -> Named<'r, S, A> {
        Named { record: self, atomic: PhantomData }
    }
}

/// Named is an atomic of a record that takes a page's name at the page's link.
pub struct Named<'r, S, A> {
    record: Record<'r, S>,
    atomic: Marks<A>,
}

impl<S: Shape, A: Atomic<S>> Named<'_, S, A> {
    /// store writes the atomic, whatever it held; it can't fail.
    pub(crate) fn store(&self, value: u64) {
        stored(self.record.place, A::INDEX, value);
    }
}

/// stored writes an atomic of the record at a place, whatever it held, by exchanges from the value it last saw.
pub(crate) fn stored(place: u64, atomic: usize, value: u64) {
    let host = current();
    let mut held = host.atomic(place, atomic, Op::Load);
    loop {
        let before = host.atomic(place, atomic, Op::Exchange(held, value));
        if before == held {
            return;
        }
        held = before;
    }
}

impl<'r, S: Single> Record<'r, S> {
    /// word returns a word of the record's run, refusing an index past the run.
    pub fn word(self, index: usize) -> Result<RunWord<'r, S>, Error> {
        if index >= current().shared_atomics(self.place) - S::ATOMICS {
            return Err(Error::fork(b"XX000", "a word of a single record's run is past the run".to_owned()));
        }
        Ok(RunWord { record: self, atomic: S::ATOMICS + index })
    }
}

/// RunWord is a word of a single record's run, an atomic that takes a page's name at the page's link.
pub struct RunWord<'r, S> {
    record: Record<'r, S>,
    /// atomic is the word's index among the record's atomics.
    atomic: usize,
}

impl<S> RunWord<'_, S> {
    /// load reads the word.
    pub fn load(&self) -> u64 {
        current().atomic(self.record.place, self.atomic, Op::Load)
    }

    /// store writes the word, whatever it held.
    pub fn store(&self, value: u64) {
        stored(self.record.place, self.atomic, value);
    }

    /// compare_exchange writes a value if the word holds the one expected, returning the value it held.
    pub fn compare_exchange(&self, expected: u64, value: u64) -> u64 {
        current().atomic(self.record.place, self.atomic, Op::Exchange(expected, value))
    }

    /// fetch_add adds to the word, wrapping, and returns the value it held.
    pub fn fetch_add(&self, value: u64) -> u64 {
        current().atomic(self.record.place, self.atomic, Op::Add(value))
    }
}

/// name returns the word a slots array holds for the record at a location it names: one past the location, so that
/// zero names no record.
fn name(location: u64) -> u64 {
    location + 1
}

impl<S: Single> Record<'_, S> {
    /// names_barrier reports whether a slots array names the record a barrier's guard is of.
    fn names_barrier<const I: usize, M>(self, barrier: &LongGuard<'_, Of<S, I>, Barrier<S, I>, M>) -> bool
    where
        S: ArrayAt<I, By: Barred>,
    {
        barrier.host.slots_name(self.place, I) == name(barrier.host.shared_location(barrier.place))
    }

    /// mark names the record of a barrier, held exclusively, as the one whose barrier guards a slots array, and
    /// empties the array, reporting whether it did: it does when the array names no record, or one whose slot was
    /// given back since, and changes nothing when the array names a record that still has its slot, or when another
    /// mark names its record first. The fork's tests hold it right after its load of the name (012_slots.pl: the
    /// mark that lost the name answers false).
    pub fn mark<const I: usize>(self, barrier: &mut LongGuard<'_, Of<S, I>, Barrier<S, I>>) -> Result<bool, Error>
    where
        S: ArrayAt<I, By: Barred>,
    {
        let host = barrier.host;
        let loaded = host.slots_name(self.place, I);
        crate::pause!(barrier, c"doltgres-shared-marking")?;
        if loaded != 0 && host.shared_live(loaded - 1) {
            return Ok(false);
        }
        let mine = name(host.shared_location(barrier.place));
        if !host.slots_rename(self.place, I, loaded, mine) {
            return Ok(false);
        }
        host.slots_empty(self.place, I);
        Ok(true)
    }

    /// named returns the location of the record a slots array names, or nothing when it names none.
    pub fn named<const I: usize>(self) -> Option<u64>
    where
        S: ArrayAt<I, By: Barred>,
    {
        current().slots_name(self.place, I).checked_sub(1)
    }

    /// unmark clears a slots array's name by one swap from the name of the barrier's record, refusing a barrier of a
    /// record the array doesn't name.
    pub fn unmark<const I: usize>(self, barrier: &mut LongGuard<'_, Of<S, I>, Barrier<S, I>>) -> Result<(), Error>
    where
        S: ArrayAt<I, By: Barred>,
    {
        let mine = name(barrier.host.shared_location(barrier.place));
        if !barrier.host.slots_rename(self.place, I, mine, 0) {
            return Err(unnamed());
        }
        Ok(())
    }

    /// read returns the claimed slots of a slots array, refusing a barrier of a record the array doesn't name.
    pub fn read<const I: usize>(
        self,
        barrier: &mut LongGuard<'_, Of<S, I>, Barrier<S, I>>,
    ) -> Result<Vec<Vec<u8>>, Error>
    where
        S: ArrayAt<I, By: Barred>,
    {
        if !self.names_barrier::<I, _>(barrier) {
            return Err(unnamed());
        }
        Ok(barrier.host.slots_read(self.place, I))
    }

    /// claim claims a slot of a slots array for an entry's bytes, reporting whether it did: it claims none through a
    /// barrier of a record the array doesn't name, and none once every slot is claimed.
    pub fn claim<const I: usize>(
        self,
        barrier: &LongGuard<'_, Of<S, I>, Barrier<S, I>, Shared>,
        bytes: &<S as ArrayAt<I>>::Entry,
    ) -> Result<bool, Error>
    where
        S: ArrayAt<I, By: Barred>,
    {
        if !self.names_barrier::<I, _>(barrier) {
            return Ok(false);
        }
        barrier.host.slots_claim(&MayRaise::of(), self.place, I, bytes.bytes())
    }
}

/// unnamed returns the error of a slots array reached through a barrier of a record it doesn't name.
fn unnamed() -> Error {
    Error::fork(b"XX000", "a slots array doesn't name the record of the barrier it is reached through".to_owned())
}

/// take takes a record's lock.
fn take(host: &'static dyn Host, place: u64, lock: usize, exclusive: bool) -> Result<(), Error> {
    host.shared_lock(&MayRaise::of(), place, lock, exclusive)
}

/// Guard is a held lock under which no file I/O runs, in a mode, which lends its taker's lock scope.
pub struct Guard<'a, S, L, M> {
    /// host is the host that took the lock, which releases it.
    host: &'static dyn Host,
    place: u64,
    /// index is the lock's place among the record's locks, which the guard releases.
    index: usize,
    held: &'a mut Locks,
    lock: Marks<(S, L, M)>,
}

impl<'a, S: Shape, L, M> Guard<'a, S, L, M> {
    /// new returns the guard of a lock just taken.
    fn new(place: u64, index: usize, held: &'a mut Locks) -> Guard<'a, S, L, M> {
        Guard { host: current(), place, index, held, lock: PhantomData }
    }

    /// held lends the taker's lock scope, for backend locks under this one.
    pub fn held(&mut self) -> &mut Locks {
        self.held
    }

    /// place returns the place of the record whose lock the guard holds.
    pub(crate) fn place(&self) -> u64 {
        self.place
    }

    /// record returns the record whose lock the guard holds, reached for as long as the guard is.
    pub fn record(&self) -> Record<'_, S> {
        Record::at(self.place)
    }

    /// lock takes a later lock of the same record exclusively.
    pub fn lock<N: Lock<S> + After<L>>(&mut self) -> Result<Guard<'_, S, N, Exclusive>, Error> {
        take(self.host, self.place, N::INDEX, true)?;
        Ok(Guard::new(self.place, N::INDEX, self.held))
    }

    /// lock_shared takes a later lock of the same record shared.
    pub fn lock_shared<N: Lock<S> + After<L>>(&mut self) -> Result<Guard<'_, S, N, Shared>, Error> {
        take(self.host, self.place, N::INDEX, false)?;
        Ok(Guard::new(self.place, N::INDEX, self.held))
    }

    /// read_blob returns the bytes of a blob this lock guards.
    pub fn read_blob<const I: usize>(&mut self) -> Vec<u8>
    where
        S: BlobAt<I, Guard = L>,
    {
        self.host.blob_read(self.place, I)
    }

    /// entry returns the bytes of an entry of an array this lock guards, refusing an index past the array's count.
    pub fn entry<const I: usize>(&mut self, index: usize) -> Result<Vec<u8>, Error>
    where
        S: ArrayAt<I, By = Entries<L>>,
    {
        self.host.entry_read(self.place, I, index)
    }
}

impl<S: Shape, L> Guard<'_, S, L, Exclusive> {
    /// write_blob writes the bytes of a blob this lock, held exclusively, guards, in place, refusing bytes longer than
    /// the blob's largest size with nothing written.
    pub fn write_blob<const I: usize>(&mut self, bytes: &[u8]) -> Result<(), Error>
    where
        S: BlobAt<I, Guard = L>,
    {
        self.host.blob_write(self.place, I, bytes)
    }

    /// set_entry writes the bytes of an entry of an array this lock, held exclusively, guards, in place, refusing an
    /// index past the array's count with nothing written.
    pub fn set_entry<const I: usize>(&mut self, index: usize, bytes: &<S as ArrayAt<I>>::Entry) -> Result<(), Error>
    where
        S: ArrayAt<I, By = Entries<L>>,
    {
        self.host.entry_write(self.place, I, index, bytes.bytes())
    }
}

impl<S, L, M> Drop for Guard<'_, S, L, M> {
    fn drop(&mut self) {
        self.host.shared_unlock(self.place, self.index);
    }
}

/// LongGuard is a lock held across I/O, in a mode, which lends a capability that does file I/O but never waits.
pub struct LongGuard<'a, S, L, M = Exclusive> {
    /// host is the host that took the lock, which releases it.
    host: &'static dyn Host,
    place: u64,
    /// index is the lock's place among the record's locks, which the guard releases.
    index: usize,
    held: &'a mut Locks,
    lock: Marks<(S, L, M)>,
}

impl<'a, S: Shape, L, M> LongGuard<'a, S, L, M> {
    /// new returns the guard of a long lock just taken.
    fn new(place: u64, index: usize, held: &'a mut Locks) -> LongGuard<'a, S, L, M> {
        LongGuard { host: current(), place, index, held, lock: PhantomData }
    }

    /// io lends the capability for the lock's file I/O and for later locks of the same record.
    pub fn io(&mut self) -> LongIo<'_, S, L> {
        LongIo { place: self.place, held: self.held, lock: PhantomData }
    }

    /// read_blob returns the bytes of a blob this lock guards.
    pub fn read_blob<const I: usize>(&mut self) -> Vec<u8>
    where
        S: BlobAt<I, Guard = L>,
    {
        self.host.blob_read(self.place, I)
    }
}

impl<S: Shape, L> LongGuard<'_, S, L> {
    /// write_blob writes the bytes of a blob this lock, held exclusively, guards, in place, refusing bytes longer than
    /// the blob's largest size with nothing written.
    pub fn write_blob<const I: usize>(&mut self, bytes: &[u8]) -> Result<(), Error>
    where
        S: BlobAt<I, Guard = L>,
    {
        self.host.blob_write(self.place, I, bytes)
    }
}

impl<S, L, M> Drop for LongGuard<'_, S, L, M> {
    fn drop(&mut self) {
        self.host.shared_unlock(self.place, self.index);
    }
}

/// LongIo is a long lock's capability: it does file I/O and takes later locks of the same record, short and long, but
/// never waits on a flag.
pub struct LongIo<'a, S, L> {
    place: u64,
    held: &'a mut Locks,
    lock: Marks<(S, L)>,
}

impl<S: Shape, L> LongIo<'_, S, L> {
    /// lock takes a later lock of the same record exclusively.
    pub fn lock<N: Lock<S> + After<L>>(&mut self) -> Result<Guard<'_, S, N, Exclusive>, Error> {
        take(current(), self.place, N::INDEX, true)?;
        Ok(Guard::new(self.place, N::INDEX, self.held))
    }

    /// lock_shared takes a later lock of the same record shared.
    pub fn lock_shared<N: Lock<S> + After<L>>(&mut self) -> Result<Guard<'_, S, N, Shared>, Error> {
        take(current(), self.place, N::INDEX, false)?;
        Ok(Guard::new(self.place, N::INDEX, self.held))
    }

    /// lock_long takes a later lock of the same record that is held across file I/O, exclusively.
    pub fn lock_long<N: LongLock<S> + After<L>>(&mut self) -> Result<LongGuard<'_, S, N>, Error> {
        take(current(), self.place, N::INDEX, true)?;
        Ok(LongGuard::new(self.place, N::INDEX, self.held))
    }

    /// lock_long_shared takes a later lock of the same record that is held across file I/O, shared.
    pub fn lock_long_shared<N: LongLock<S> + After<L>>(&mut self) -> Result<LongGuard<'_, S, N, Shared>, Error> {
        take(current(), self.place, N::INDEX, false)?;
        Ok(LongGuard::new(self.place, N::INDEX, self.held))
    }
}

impl<S, L> std::ops::Deref for LongIo<'_, S, L> {
    type Target = Locks;

    fn deref(&self) -> &Locks {
        self.held
    }
}

impl<S, L> std::ops::DerefMut for LongIo<'_, S, L> {
    fn deref_mut(&mut self) -> &mut Locks {
        self.held
    }
}

impl<S, L> sealed::Sealed for LongIo<'_, S, L> {
    fn parts(&mut self) -> &mut Locks {
        self.held
    }
}

impl<S, L> ReadDoors for LongIo<'_, S, L> {}

impl<S, L> IoDoors for LongIo<'_, S, L> {}

/// FlagGuard is a set flag, which lends a capability for the long operation it marks.
pub struct FlagGuard<'a, S, F> {
    /// host is the host that set the flag, which clears it.
    host: &'static dyn Host,
    place: u64,
    /// index is the flag's place among the record's flags, which the guard clears.
    index: usize,
    held: &'a mut Locks,
    flag: Marks<(S, F)>,
}

impl<S: Shape, F: Flag<S>> FlagGuard<'_, S, F> {
    /// io lends the capability for the flag's operation.
    pub fn io(&mut self) -> FlagIo<'_, S, F> {
        FlagIo { place: self.place, held: self.held, flag: PhantomData }
    }
}

impl<S, F> Drop for FlagGuard<'_, S, F> {
    fn drop(&mut self) {
        self.host.flag_clear(self.place, self.index);
    }
}

/// FlagIo is a flag's capability: it does file I/O, checks interrupts, takes later flags, and takes any lock of the
/// same record, but lends no raw `Io`.
pub struct FlagIo<'a, S, F> {
    place: u64,
    held: &'a mut Locks,
    flag: Marks<(S, F)>,
}

impl<S: Shape, F> FlagIo<'_, S, F> {
    /// check fails with Postgres' error once an interrupt is pending.
    pub fn check(&mut self) -> Result<(), Error> {
        current().check_interrupts(&MayRaise::of())
    }

    /// flag sets a later flag of the same record.
    pub fn flag<G: Flag<S> + After<F>>(&mut self) -> Result<FlagGuard<'_, S, G>, Error> {
        current().flag_take(&MayRaise::of(), self.place, G::INDEX)?;
        Ok(FlagGuard { host: current(), place: self.place, index: G::INDEX, held: self.held, flag: PhantomData })
    }

    /// lock takes a lock of the same record under which no file I/O runs, exclusively.
    pub fn lock<L: Lock<S>>(&mut self) -> Result<Guard<'_, S, L, Exclusive>, Error> {
        take(current(), self.place, L::INDEX, true)?;
        Ok(Guard::new(self.place, L::INDEX, self.held))
    }

    /// lock_shared takes a lock of the same record under which no file I/O runs, shared.
    pub fn lock_shared<L: Lock<S>>(&mut self) -> Result<Guard<'_, S, L, Shared>, Error> {
        take(current(), self.place, L::INDEX, false)?;
        Ok(Guard::new(self.place, L::INDEX, self.held))
    }

    /// lock_long takes a lock of the same record held across file I/O.
    pub fn lock_long<L: LongLock<S>>(&mut self) -> Result<LongGuard<'_, S, L>, Error> {
        take(current(), self.place, L::INDEX, true)?;
        Ok(LongGuard::new(self.place, L::INDEX, self.held))
    }
}

impl<S, F> std::ops::Deref for FlagIo<'_, S, F> {
    type Target = Locks;

    fn deref(&self) -> &Locks {
        self.held
    }
}

impl<S, F> std::ops::DerefMut for FlagIo<'_, S, F> {
    fn deref_mut(&mut self) -> &mut Locks {
        self.held
    }
}

impl<S, F> sealed::Sealed for FlagIo<'_, S, F> {
    fn parts(&mut self) -> &mut Locks {
        self.held
    }
}

impl<S, F> ReadDoors for FlagIo<'_, S, F> {}

impl<S, F> IoDoors for FlagIo<'_, S, F> {}

/// Epochs is the fork-wide epoch lock, held exclusively, never with a record's lock.
pub struct Epochs<'a> {
    /// host is the host that took the epoch lock, which releases it.
    host: &'static dyn Host,
    held: PhantomData<&'a mut Locks>,
}

/// epochs takes the epoch lock exclusively, to queue what a reader may still see or to compare the readers' epochs.
pub fn epochs<'a>(_: &'a mut Io) -> Result<Epochs<'a>, Error> {
    current().epochs_lock(&MayRaise::of())?;
    Ok(Epochs { host: current(), held: PhantomData })
}

impl Epochs<'_> {
    /// advance returns the current epoch, at which what is queued now waits, and advances the current epoch past it.
    pub fn advance(&mut self) -> u64 {
        self.host.epochs_advance()
    }

    /// oldest returns the oldest epoch a live reader started at, or the current epoch when no reader is live.
    pub fn oldest(&mut self) -> u64 {
        self.host.epochs_oldest()
    }
}

impl Drop for Epochs<'_> {
    fn drop(&mut self) {
        self.host.epochs_unlock();
    }
}

/// Reader is a reader's epoch, published until it drops, which keeps what the reader may see from going.
pub struct Reader {
    /// host is the host that started the reader, which ends it.
    host: &'static dyn Host,
    epoch: u64,
}

/// read starts a reader at the current epoch, published before the reader reads what the epochs keep.
pub fn read(io: &mut Io) -> Result<Reader, Error> {
    let host = current();
    Ok(Reader { host, epoch: host.epochs_read(&doors(io))? })
}

impl Reader {
    /// epoch returns the epoch the reader started at.
    pub fn epoch(&self) -> u64 {
        self.epoch
    }
}

impl Drop for Reader {
    fn drop(&mut self) {
        self.host.epochs_end(self.epoch);
    }
}

/// proc_number returns the process's number among the server's processes, which shared entries it owns name.
pub fn proc_number() -> u32 {
    current().proc_number()
}

/// Mine is the running process's handle of its own slot of words, which stays with the process that got it.
pub struct Mine<P> {
    /// host is the host whose process the slot is of.
    host: &'static dyn Host,
    place: u64,
    slot: Marks<P>,
    /// unsent keeps the handle from being sent to another thread.
    unsent: PhantomData<*const ()>,
}

/// mine returns the running process's handle of its own slot of words. Its caller shows the entry's `Io` or a
/// registration's view of it, of which it reads nothing.
pub fn mine<P: ProcSlots>(_: &mut impl TakesShort) -> Result<Mine<P>, Error> {
    let host = current();
    let place = host.words_mine(&Kind::slots::<P>())?;
    Ok(Mine { host, place, slot: PhantomData, unsent: PhantomData })
}

impl<P: ProcSlots> Mine<P> {
    /// store stores one of the process's words.
    pub fn store<W: Word<P>>(&self, value: u64) {
        self.host.word_store(self.place, W::INDEX, value);
    }

    /// load loads one of the process's words.
    pub fn load<W: Word<P>>(&self) -> u64 {
        self.host.atomic(self.place, W::INDEX, Op::Load)
    }

    /// exchange stores one of the process's words over the value given, returning the value it found.
    pub fn exchange<W: Word<P>>(&self, loaded: u64, value: u64) -> u64 {
        self.host.atomic(self.place, W::INDEX, Op::Exchange(loaded, value))
    }

    /// id returns a number naming the process among the slots' holders, for a word another process stores.
    pub fn id(&self) -> u64 {
        self.place
    }
}

/// processes returns the numbers of the processes whose slot can hold a word, each of which `load` takes. A process
/// that takes its slot after the list was made isn't in it.
pub fn processes<P: ProcSlots>() -> Result<Vec<u32>, Error> {
    current().words_processes(&Kind::slots::<P>())
}

/// load loads one word of a process's slot, refusing a process number past the server's processes.
pub fn load<P: ProcSlots, W: Word<P>>(proc: u32) -> Result<u64, Error> {
    current().word_load(&Kind::slots::<P>(), proc, W::INDEX)
}

/// Theirs is another process's slot of a kind of words, found once, so a store into one of its words can't fail.
pub struct Theirs<P> {
    /// host is the host whose process the slot is of.
    host: &'static dyn Host,
    place: u64,
    slot: Marks<P>,
    /// unsent keeps the handle from being sent to another thread.
    unsent: PhantomData<*const ()>,
}

/// theirs finds a process's slot of a kind of words, refusing a process with none.
pub fn theirs<P: ProcSlots>(proc: u32) -> Result<Theirs<P>, Error> {
    let host = current();
    Ok(Theirs { host, place: host.words_place(&Kind::slots::<P>(), proc)?, slot: PhantomData, unsent: PhantomData })
}

impl<P: ProcSlots> Theirs<P> {
    /// exchange stores one of the process's words over the value given, returning the value it found.
    pub fn exchange<W: Word<P>>(&self, loaded: u64, value: u64) -> u64 {
        self.host.atomic(self.place, W::INDEX, Op::Exchange(loaded, value))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// line returns a table's line of a kind of an id, laid out as no shape needs.
    const fn line(id: u64, form: Form) -> Kind {
        Kind { id, locks: 0, flags: 0, atomics: 0, blobs: &[], arrays: &[], form }
    }

    /// aged returns the line of an aged kind of an id whose records name the kinds of the ids given.
    const fn aged(id: u64, names: &'static [u64]) -> Kind {
        line(id, Form::Aged { count: |_| 1, names })
    }

    /// A table lists an aged kind before each aged kind it names, and names only aged kinds it lists.
    #[test]
    fn a_table_lists_an_aged_kind_before_what_it_names() {
        let single = line(3, Form::Single { run: |_| 0 });
        assert!(ordered(&[aged(1, &[2]), aged(2, &[]), line(3, Form::Single { run: |_| 0 })]));
        assert!(!ordered(&[aged(2, &[]), aged(1, &[2])]));
        assert!(!ordered(&[aged(1, &[4]), aged(2, &[])]));
        assert!(!ordered(&[aged(1, &[3]), single]));
        assert!(!ordered(&[aged(1, &[1])]));
    }
}
