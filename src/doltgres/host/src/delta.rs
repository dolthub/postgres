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

//! The delta's pages (design item 14's S6): the memory of the delta layer as pages of bytes written once and of
//! words, which every backend shares. A page is its allocator's own until a link names it, is read and appended to
//! under a hold, and is freed by a pass once every hold that began before its retirement has ended. No door reads
//! or writes memory a page doesn't own, whatever the names a crate hands it.

use std::marker::PhantomData;
use std::ops::Deref;

use crate::io::Io;
use crate::shared::{Aged, Atomic, Exclusive, Guard, Kind, Lock, Named, Op, Record, RunWord, Shape};
use crate::{Error, Host, cache, current, doors};

pub mod blocks;
pub mod directory;
pub mod list;
pub mod page;

use directory::{Opened, Owner};
pub use page::{Page, Size};

/// Entry binds a handle to the entry whose `Io` made it, and keeps it from being sent.
type Entry<'io> = PhantomData<(&'io (), *const ())>;

/// WordOp is an operation on one of a page's words.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WordOp {
    /// Load reads the word.
    Load,
    /// Store writes the word, and answers zero.
    Store(u64),
    /// Exchange writes the second value if the first is the word's, returning the value before.
    Exchange(u64, u64),
    /// Add adds to the word, wrapping, returning the value before.
    Add(u64),
}

/// Hold is a reader of the fork-wide epochs for the entry that took it: a page retired after it began isn't freed
/// until it ends, and one retired before it began isn't opened under it.
pub struct Hold<'io> {
    /// host is the host that began the hold, which ends it.
    host: &'static dyn Host,
    epoch: u64,
    entry: Entry<'io>,
}

/// hold begins a hold for the running entry.
pub fn hold<'io>(io: &mut Io) -> Result<Hold<'io>, Error> {
    let host = current();
    Ok(Hold { host, epoch: host.delta_hold(&doors(io))?, entry: PhantomData })
}

impl Hold<'_> {
    /// page opens the page a name names, for as long as the hold lasts, refusing a name that names no page the hold
    /// reaches: a number past the directory, another generation, a page neither live nor retired, or one retired
    /// before the hold began.
    pub fn page(&self, name: Page) -> Result<PageRef<'_>, Error> {
        Ok(PageRef { host: self.host, opened: self.host.delta_open(name, self.epoch)?, hold: PhantomData })
    }
}

impl Hold<'_> {
    /// record opens the record of an aged shape a location names, for as long as the hold lasts, refusing a location
    /// that names no record the hold reaches, as `page` refuses a name.
    pub fn record<S: Aged>(&self, location: u64) -> Result<Record<'_, S>, Error> {
        Ok(Record::at(self.host.delta_record(&Kind::aged::<S>(), location, self.epoch)?))
    }
}

impl Drop for Hold<'_> {
    fn drop(&mut self) {
        self.host.delta_release(self.epoch);
    }
}

/// PageRef is a page opened under a hold: the one way to its bytes, its words, its appends, and its seal.
pub struct PageRef<'h> {
    /// host is the host that opened the page.
    host: &'static dyn Host,
    opened: Opened,
    hold: PhantomData<&'h ()>,
}

/// owner returns the lock a guard holds, as a page's slot records the lock that may append to it.
fn owner<S: Shape, L: Lock<S>>(host: &dyn Host, guard: &Guard<'_, S, L, Exclusive>) -> Owner {
    Owner { shape: S::ID, location: host.shared_location(guard.place()), lock: L::INDEX as u64 }
}

impl<'h> PageRef<'h> {
    /// bytes lends a range of the page's bytes below its filled mark, loaded at the call, for as long as the hold
    /// lasts.
    pub fn bytes(&self, offset: u64, len: u64) -> Result<Lent<'h>, Error> {
        Ok(Lent { bytes: self.host.delta_bytes(&self.opened, offset, len)?, hold: PhantomData })
    }

    /// room returns how many bytes the page takes above its filled mark, loaded at the call, and none for a sealed
    /// page.
    pub fn room(&self) -> u64 {
        self.host.delta_room(&self.opened)
    }

    /// filled returns how many bytes the page holds below its filled mark, loaded at the call.
    pub fn filled(&self) -> u64 {
        self.host.delta_filled(&self.opened)
    }

    /// words returns the page's count of words.
    pub fn words(&self) -> u64 {
        u64::from(self.opened.size.words)
    }

    /// word returns one of the page's words, refusing an index past its count.
    pub fn word(&self, index: u64) -> Result<Word<'h>, Error> {
        directory::worded(self.opened.size, index)?;
        Ok(Word { host: self.host, body: self.opened.body, index, hold: PhantomData })
    }

    /// append copies a record above the page's filled mark and then advances the mark, returning the record's
    /// offset. Its guard is the exclusive guard of the lock the page's slot records, so another record's guard and
    /// a sealed page are refused, as is a record that doesn't fit, which changes nothing.
    pub fn append<S: Shape, L: Lock<S>>(
        &self,
        guard: &mut Guard<'_, S, L, Exclusive>,
        record: &[u8],
    ) -> Result<u32, Error> {
        self.host.delta_append(&self.opened, owner(self.host, guard), record)
    }

    /// seal clears the page's owner under the guard of the lock its slot records, after which the page takes no
    /// append; a seal of a page already sealed does nothing, since a split that fails after its seal leaves its range's
    /// own pages sealed for the next split of the range to seal again.
    pub fn seal<S: Shape, L: Lock<S>>(&self, guard: &mut Guard<'_, S, L, Exclusive>) -> Result<(), Error> {
        self.host.delta_seal(&self.opened, owner(self.host, guard))
    }
}

/// Lent is a range of a page's bytes lent for as long as the hold that opened the page: the server's is the page's
/// own memory, which is never written again below the mark, and the simulation's is a copy.
pub struct Lent<'h> {
    bytes: cache::Bytes,
    hold: PhantomData<&'h ()>,
}

impl Deref for Lent<'_> {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.bytes
    }
}

/// Word is one of the words of a page opened under a hold, which changes after the page is published.
pub struct Word<'h> {
    /// host is the host that opened the word's page.
    host: &'static dyn Host,
    body: u64,
    index: u64,
    hold: PhantomData<&'h ()>,
}

impl Word<'_> {
    /// load reads the word.
    pub fn load(&self) -> u64 {
        self.host.delta_word(self.body, self.index, WordOp::Load)
    }

    /// store writes the word.
    pub fn store(&self, value: u64) {
        self.host.delta_word(self.body, self.index, WordOp::Store(value));
    }

    /// compare_exchange writes a value if the word holds the one expected, returning the value it held.
    pub fn compare_exchange(&self, expected: u64, value: u64) -> u64 {
        self.host.delta_word(self.body, self.index, WordOp::Exchange(expected, value))
    }

    /// fetch_add adds to the word, wrapping, and returns the value it held.
    pub fn fetch_add(&self, value: u64) -> u64 {
        self.host.delta_word(self.body, self.index, WordOp::Add(value))
    }
}

/// names keeps what takes a page's name at its link to the words this crate hands out.
mod names {
    /// Takes stores a page's name.
    pub trait Takes {
        /// take stores the name; it can't fail.
        fn take(&self, name: u64);
    }
}

/// Names is a word that takes a page's name at the page's link: a word of a page, an atomic of a record, or a word
/// of a single record's run.
pub trait Names: names::Takes {}

impl names::Takes for Word<'_> {
    fn take(&self, name: u64) {
        self.store(name);
    }
}

impl Names for Word<'_> {}

impl<S: Shape, A: Atomic<S>> names::Takes for Named<'_, S, A> {
    fn take(&self, name: u64) {
        self.store(name);
    }
}

impl<S: Shape, A: Atomic<S>> Names for Named<'_, S, A> {}

impl<S> names::Takes for RunWord<'_, S> {
    fn take(&self, name: u64) {
        self.store(name);
    }
}

impl<S> Names for RunWord<'_, S> {}

/// Owned is a page its allocator owns: no open reaches it, so its bytes and words are its owner's alone until its
/// link. One dropped unlinked gives its slot and its memory back, and none outlives the entry that allocated it.
pub struct Owned<'io> {
    /// host is the host that allocated the page.
    host: &'static dyn Host,
    name: Page,
    entry: Entry<'io>,
}

impl Owned<'_> {
    /// name returns the page's name, which its link will store.
    pub fn name(&self) -> Page {
        self.name
    }

    /// append copies a record above the page's filled mark and returns its offset, refusing one that doesn't fit.
    pub fn append(&mut self, record: &[u8]) -> Result<u32, Error> {
        self.host.delta_owned_append(self.name, record)
    }

    /// store writes one of the page's words, refusing an index past its count.
    pub fn store(&mut self, index: u64, value: u64) -> Result<(), Error> {
        self.host.delta_owned_store(self.name, index, value)
    }

    /// link records the guard's lock as the one that may append to the page, makes the page live, and stores its
    /// name in the given word, with nothing that can fail between them.
    pub fn link<S: Shape, L: Lock<S>>(self, guard: &mut Guard<'_, S, L, Exclusive>, word: &impl Names) -> Page {
        self.host.delta_link(self.name, owner(self.host, guard));
        word.take(self.name.value());
        self.name
    }
}

impl Drop for Owned<'_> {
    fn drop(&mut self) {
        self.host.delta_give_back(self.name);
    }
}

/// Unlinked is a record of an aged kind its maker owns: nothing names it, so its parts are its maker's alone until its
/// link. One dropped unlinked gives its slot back at once, which counts as a free, and none outlives the entry that
/// made it.
struct Unlinked<'io> {
    /// host is the host that made the record.
    host: &'static dyn Host,
    place: u64,
    entry: Entry<'io>,
}

impl Drop for Unlinked<'_> {
    fn drop(&mut self) {
        self.host.delta_unmake(self.place);
    }
}

/// Made is a record of an aged shape its maker owns until its link: its location, its atomics, and the words that
/// take a name, and no lock, since nothing that takes a lock may reach a record no word names.
pub struct Made<'io, S> {
    unlinked: Unlinked<'io>,
    location: u64,
    shape: PhantomData<fn() -> S>,
}

/// make makes a record of an aged shape, its parts cleared, which its maker owns until its link. A refusal is the
/// budget's signal, as `alloc`'s is: the maker drops what it owns, waits, and asks again.
pub fn make<'io, S: Aged>(io: &mut Io) -> Result<Result<Made<'io, S>, Full>, Error> {
    let host = current();
    Ok(match host.delta_make(&doors(io), &Kind::aged::<S>(), S::NAME)? {
        Ok((place, location)) => {
            Ok(Made { unlinked: Unlinked { host, place, entry: PhantomData }, location, shape: PhantomData })
        }
        Err(frees) => Err(Full { frees }),
    })
}

impl<'io, S: Aged> Made<'io, S> {
    /// location returns the record's location, which its link will store.
    pub fn location(&self) -> u64 {
        self.location
    }

    /// atomic operates on one of the record's atomics, returning its value before.
    pub fn atomic<A: Atomic<S>>(&self, op: Op) -> u64 {
        Record::<S>::at(self.unlinked.place).atomic::<A>(op)
    }

    /// names returns one of the record's atomics as a word that takes a name at a link.
    pub fn names<A: Atomic<S>>(&self) -> Named<'_, S, A> {
        Record::at(self.unlinked.place).names::<A>()
    }

    /// link makes live every page the given `With` holds, each owned by the lock it was given with, publishes every
    /// record it holds and then this record, and stores this record's location in the given word, returning it.
    /// Nothing in it can fail, so no error and no exit falls between its stores.
    pub fn link(self, word: &impl Names, with: With<'io, S>) -> u64 {
        let host = self.unlinked.host;
        for (page, lock) in &with.own {
            host.delta_link(page.name, Owner { shape: S::ID, location: self.location, lock: *lock });
        }
        for (page, owner) in &with.pages {
            host.delta_link(page.name, *owner);
        }
        for record in &with.records {
            host.delta_publish(record.place);
        }
        host.delta_publish(self.unlinked.place);
        word.take(self.location);
        self.location
    }
}

/// With is what a record's link makes live with it, gathered before the link: pages the record's own locks will
/// own, and unlinked records under it with the pages their locks will own. So a page goes live only with an owner
/// that the same link publishes.
pub struct With<'io, S> {
    /// own are the pages the linked record's locks will own, each with its lock's index.
    own: Vec<(Owned<'io>, u64)>,
    /// pages are the pages the records under it will own, each with its owner.
    pages: Vec<(Owned<'io>, Owner)>,
    records: Vec<Unlinked<'io>>,
    shape: PhantomData<fn() -> S>,
}

impl<S> Default for With<'_, S> {
    fn default() -> Self {
        With { own: Vec::new(), pages: Vec::new(), records: Vec::new(), shape: PhantomData }
    }
}

impl<'io, S: Aged> With<'io, S> {
    /// page adds a page the linked record's lock `L` will own.
    pub fn page<L: Lock<S>>(&mut self, page: Owned<'io>) -> Result<(), Error> {
        self.own.try_reserve(1).map_err(|_| Error::out_of_memory())?;
        self.own.push((page, L::INDEX as u64));
        Ok(())
    }

    /// record adds an unlinked record under the linked one, with the pages its lock `L` will own.
    pub fn record<R: Aged, L: Lock<R>>(
        &mut self,
        made: Made<'io, R>,
        pages: impl IntoIterator<Item = Owned<'io>>,
    ) -> Result<(), Error> {
        let owner = Owner { shape: R::ID, location: made.location, lock: L::INDEX as u64 };
        self.records.try_reserve(1).map_err(|_| Error::out_of_memory())?;
        self.records.push(made.unlinked);
        for page in pages {
            self.pages.try_reserve(1).map_err(|_| Error::out_of_memory())?;
            self.pages.push((page, owner));
        }
        Ok(())
    }
}

/// Leaving is a record of an aged kind that a retirement takes with its pages: its shape's id and its location.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Leaving {
    id: u64,
    location: u64,
}

impl Leaving {
    /// of returns the record of an aged shape at a location.
    pub fn of<S: Aged>(location: u64) -> Leaving {
        Leaving { id: S::ID, location }
    }

    /// raw returns the record of the shape with an id at a location, for simc's C door, whose shapes' ids come at run
    /// time.
    #[cfg(feature = "testing")]
    pub fn raw(id: u64, location: u64) -> Leaving {
        Leaving { id, location }
    }
}

/// Full is the budget's answer to an allocation the delta can't give now: the count of frees the allocation loaded
/// before it tried, which the wait takes. Only a refusal makes one, and it is neither copied nor made by a crate.
pub struct Full {
    frees: u64,
}

/// alloc takes every page a write may need, all of them or none. A refusal is the budget's signal, never a user's
/// error: the writer drops what it owns, waits, and asks again. Its one error of a size is one an empty delta
/// couldn't give.
pub fn alloc<'io>(io: &mut Io, sizes: &[Size]) -> Result<Result<Vec<Owned<'io>>, Full>, Error> {
    let host = current();
    let (mut names, mut owned) = (Vec::new(), Vec::new());
    names.try_reserve_exact(sizes.len()).map_err(|_| Error::out_of_memory())?;
    owned.try_reserve_exact(sizes.len()).map_err(|_| Error::out_of_memory())?;
    if let Some(frees) = host.delta_alloc(&doors(io), sizes, &mut names)? {
        return Ok(Err(Full { frees }));
    }
    owned.extend(names.into_iter().map(|name| Owned { host, name, entry: PhantomData }));
    Ok(Ok(owned))
}

/// alloc_replacing takes a page of a size for the copy of a page it replaces, a compaction's, which the budget lets
/// one replacement at a time take past it (bugs row 110); it answers as `alloc` does.
pub fn alloc_replacing<'io>(io: &mut Io, size: Size, replaced: Page) -> Result<Result<Owned<'io>, Full>, Error> {
    let host = current();
    let mut names = Vec::new();
    names.try_reserve_exact(1).map_err(|_| Error::out_of_memory())?;
    if let Some(frees) = host.delta_alloc_replacing(&doors(io), size, replaced, &mut names)? {
        return Ok(Err(Full { frees }));
    }
    let name = names.pop().ok_or_else(|| Error::fork(b"XX000", "a replacement took no page".to_owned()))?;
    Ok(Ok(Owned { host, name, entry: PhantomData }))
}

/// wait waits at the budget for a free made since a refusal, returning at once when one was made already. It
/// refuses a process that has a hold, owns a page, or owns an unlinked record, so a waiter keeps nothing from being
/// freed; a cancel ends it with Postgres' error.
pub fn wait(io: &mut Io, full: Full) -> Result<(), Error> {
    current().delta_wait(&doors(io), full.frees)
}

/// BATCH is the most names one take of the epoch lock retires.
const BATCH: usize = 64;

/// retire retires records and the pages an iterator names, a batch at a time: each batch gets one stamp and one
/// advance of the epoch under one take of the epoch lock, the records going with the first, a record that isn't
/// published or a name that isn't live doing nothing, and the free pass runs once after the last. Its own steps can't
/// fail and check for no interrupt. An error the iterator yields ends the draining: the records and every name
/// yielded before it are retired, and the error is returned. Its caller shows the entry's `Io`, of which it reads
/// nothing, so the iterator's code reaches no door that waits or takes a lock.
pub fn retire(
    _: &mut Io,
    records: &[Leaving],
    pages: impl IntoIterator<Item = Result<Page, Error>>,
) -> Result<(), Error> {
    let host = current();
    let mut pages = pages.into_iter();
    let mut batch = [Page::named(0); BATCH];
    let mut leaving = records;
    let last = |records: &[Leaving], names: &[Page]| {
        if !records.is_empty() || !names.is_empty() {
            host.delta_retire(records, names);
        }
    };
    let yielded = 'batches: loop {
        let mut len = 0;
        while len < BATCH {
            match pages.next() {
                Some(Ok(name)) => {
                    batch[len] = name;
                    len += 1;
                }
                Some(Err(error)) => {
                    last(leaving, &batch[..len]);
                    break 'batches Err(error);
                }
                None => {
                    last(leaving, &batch[..len]);
                    break 'batches Ok(());
                }
            }
        }
        host.delta_retire(leaving, &batch);
        leaving = &[];
    };
    host.delta_pass();
    yielded
}

/// advanced advances the epoch and returns the epoch it passed, which every reader and hold begun before the call
/// started at or below and every one begun after it started above, and the oldest one a live reader or hold started
/// at, so a removal deferred at the passed epoch knows when every reader that may still read what it removes ended
/// (bugs row 124).
pub fn advanced() -> (u64, u64) {
    current().delta_advanced()
}

/// used returns the bytes of the delta in use: its owned, live, and retired pages, and a slot left freeing.
pub fn used() -> u64 {
    current().delta_used()
}

/// budget returns the delta's memory in bytes, `delta_memory`.
pub fn budget() -> u64 {
    current().delta_budget()
}

/// seed returns the server's random seed of the delta's shapes in memory, such as a skip list's heights, drawn anew
/// at each start of its shared memory; it shapes nothing stored or returned, so a seed that differs by run or platform
/// changes no result.
pub fn seed() -> [u8; 32] {
    current().delta_seed()
}
