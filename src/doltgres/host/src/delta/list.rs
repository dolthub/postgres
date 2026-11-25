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

//! A record's lists (design item 14's S6): nine words of the record, its first six entries, its root page's name,
//! its depth and reach, and its length, and past the first six a tree of 4 KiB pages of words, each entry found by its index
//! alone. A list is loaded under its record's lock and stored under it exclusively, and a closed list, which takes
//! no store, is read with no lock by its record's retirement.

use std::marker::PhantomData;

use super::{Hold, Made, Names, Owned, Page, Size, Word, names};
use crate::shared::{Aged, Exclusive, Guard, ListAt, Lock, Op, Removes, Shape, stored};
use crate::{Error, current};

/// WORDS is the number of a record's words a list takes.
pub const WORDS: usize = 9;

/// INLINE is the number of a list's entries in its record.
pub const INLINE: u64 = 6;

/// FANOUT is the number of words of a list page, each an entry or the name of a page a level down.
pub const FANOUT: u64 = 512;

/// PAGE is a list page's size: words alone.
pub const PAGE: Size = Size { bytes: 0, words: FANOUT as u32 };

/// ROOT, DEPTH, and LENGTH are the places among a list's words of its root's name, its depth and reach, and its
/// length.
const ROOT: u64 = 6;
const DEPTH: u64 = 7;
const LENGTH: u64 = 8;

/// CLOSED marks a closed list in its length word, whose other 63 bits are its length.
const CLOSED: u64 = 1 << 63;

/// REACH is the first bit of a list's reach in its depth word, below which is its depth: the reach is the most
/// entries the list ever held, below which its pages exist.
const REACH: u32 = 8;

/// LEVELS masks a list's depth in its depth word.
const LEVELS: u64 = (1 << REACH) - 1;

/// MOST is the most entries a list holds: its inline ones and a leaf page's for each slot of the largest delta.
const MOST: u64 = INLINE + FANOUT * super::page::SLOTS;

/// DEEPEST is the most levels a list has, since five hold `MOST` entries, and the depth and `MOST` fit their bits.
pub const DEEPEST: u32 = 5;

const _: () = assert!(FANOUT.pow(DEEPEST) >= MOST && DEEPEST as u64 <= LEVELS && MOST < 1 << (64 - REACH));

/// internal returns the fork's internal error with a message.
fn internal(message: &str) -> Error {
    Error::fork(b"XX000", message.to_owned())
}

/// capacity returns the number of entries past the inline ones a tree of a depth holds.
fn capacity(depth: u64) -> u64 {
    FANOUT.pow(depth as u32)
}

/// wants returns how many pages the push of the entry past a length needs at a depth: a first root, a new root and
/// a path below it when the tree is full, or the new page of each level below the root whose first entry it is.
pub fn wants(length: u64, depth: u64) -> usize {
    if length < INLINE {
        return 0;
    }
    let entry = length - INLINE;
    if depth == 0 {
        return 1;
    }
    if entry == capacity(depth) {
        return depth as usize + 1;
    }
    (1..depth).filter(|&level| entry.is_multiple_of(capacity(level))).count()
}

/// pages returns how many pages a list of a length takes past its inline entries: its leaves, and each level above
/// them up to its root.
pub fn pages(length: u64) -> usize {
    let mut below = length.saturating_sub(INLINE);
    let mut pages = 0;
    while below > 0 {
        let level = below.div_ceil(FANOUT);
        pages += level;
        below = if level == 1 { 0 } else { level };
    }
    pages as usize
}

/// digit returns the index among a page's words of the path to an entry past the inline ones at a level, the
/// leaves being level 1.
fn digit(entry: u64, level: u64) -> u64 {
    entry / capacity(level - 1) % FANOUT
}

/// List is one of a record's lists, reached through the guard of its record's lock.
struct List<S, const I: usize> {
    /// place is the place of the record whose guard reached the list.
    place: u64,
    shape: PhantomData<fn() -> S>,
}

impl<S, const I: usize> Clone for List<S, I> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<S, const I: usize> Copy for List<S, I> {}

impl<S: Shape, L, M> Guard<'_, S, L, M> {
    /// list returns one of the guarded record's lists, to read under the guard.
    pub fn list<const I: usize>(&self) -> ListView<'_, S, I>
    where
        S: ListAt<I, Lock = L>,
    {
        ListView { list: List { place: self.place(), shape: PhantomData }, guard: PhantomData }
    }

    /// closed returns one of the guarded record's lists once it is closed, so a retirement that didn't close it reads
    /// its pages, or nothing while it is open.
    pub fn closed<const I: usize>(&self) -> Option<Closed<S, I>>
    where
        S: ListAt<I, Lock = L>,
    {
        let list = List { place: self.place(), shape: PhantomData };
        (list.word(LENGTH) & CLOSED != 0).then_some(Closed { list })
    }
}

impl<'io, S: Aged> Made<'io, S> {
    /// fill_list fills one of the record's lists with its entries while the record is unlinked, which nothing
    /// reaches before its link, so it takes no lock: the first six inline and the rest on pages it takes from the
    /// spare ones, as `pages` counts them, and fills while they are owned; it returns those pages, which the record's
    /// lock owns at its link.
    pub fn fill_list<const I: usize>(
        &self,
        entries: &[u64],
        spare: &mut Vec<Owned<'io>>,
    ) -> Result<Vec<Owned<'io>>, Error>
    where
        S: ListAt<I>,
    {
        let length = entries.len() as u64;
        if length > MOST {
            return Err(internal("a list holds no more entries than the largest delta's"));
        }
        let wanted = pages(length);
        if spare.len() < wanted {
            return Err(internal("a list's fill wasn't given the pages it wants"));
        }
        let mut filled = spare.split_off(spare.len() - wanted);
        let list = List::<S, I> { place: self.unlinked.place, shape: PhantomData };
        let (inline, rest) = entries.split_at(entries.len().min(INLINE as usize));
        let (mut level, mut names) = (Vec::new(), Vec::new());
        let (mut below, mut at, mut depth): (&[u64], usize, u64) = (rest, 0, 0);
        while !below.is_empty() {
            names.clear();
            names.try_reserve(below.len().div_ceil(FANOUT as usize)).map_err(|_| Error::out_of_memory())?;
            for chunk in below.chunks(FANOUT as usize) {
                let page = &mut filled[at];
                for (word, &value) in chunk.iter().enumerate() {
                    page.store(word as u64, value)?;
                }
                names.push(page.name().value());
                at += 1;
            }
            depth += 1;
            if names.len() == 1 {
                list.set_word(ROOT, names[0]);
                break;
            }
            std::mem::swap(&mut level, &mut names);
            below = &level;
        }
        for (index, &value) in inline.iter().enumerate() {
            list.inline(index as u64).store(value);
        }
        list.set_word(DEPTH, length << REACH | depth);
        list.set_word(LENGTH, length);
        Ok(filled)
    }
}

impl<'a, S: Shape, L> Guard<'a, S, L, Exclusive> {
    /// list_mut returns one of the guarded record's lists, to read and store under the guard held exclusively.
    pub fn list_mut<const I: usize>(&mut self) -> ListMut<'_, 'a, S, L, I>
    where
        S: ListAt<I, Lock = L>,
    {
        ListMut { list: List { place: self.place(), shape: PhantomData }, guard: self }
    }
}

/// Entry is one of a list's entries: a word of its record or a word of one of its pages.
pub struct Entry<'h> {
    at: At<'h>,
}

/// At is where an entry is.
enum At<'h> {
    /// Inline is a word of the record at a place, by its index among the record's atomics.
    Inline(u64, usize, PhantomData<&'h ()>),
    /// Paged is a word of a list page.
    Paged(Word<'h>),
}

impl Entry<'_> {
    /// load reads the entry.
    pub fn load(&self) -> u64 {
        match &self.at {
            At::Inline(place, atomic, _) => current().atomic(*place, *atomic, Op::Load),
            At::Paged(word) => word.load(),
        }
    }

    /// store writes the entry.
    fn store(&self, value: u64) {
        match &self.at {
            At::Inline(place, atomic, _) => stored(*place, *atomic, value),
            At::Paged(word) => word.store(value),
        }
    }
}

impl names::Takes for Entry<'_> {
    fn take(&self, name: u64) {
        self.store(name);
    }
}

impl Names for Entry<'_> {}

impl<S: ListAt<I>, const I: usize> List<S, I> {
    /// atomic returns the index among the record's atomics of one of the list's words.
    fn atomic(&self, word: u64) -> usize {
        S::ATOMICS + WORDS * I + word as usize
    }

    /// word loads one of the list's words.
    fn word(&self, word: u64) -> u64 {
        current().atomic(self.place, self.atomic(word), Op::Load)
    }

    /// set_word stores one of the list's words.
    fn set_word(&self, word: u64, value: u64) {
        stored(self.place, self.atomic(word), value);
    }

    /// inline returns one of the list's words as an entry.
    fn inline<'h>(&self, word: u64) -> Entry<'h> {
        Entry { at: At::Inline(self.place, self.atomic(word), PhantomData) }
    }

    /// len returns the list's count of entries.
    fn len(&self) -> u64 {
        self.word(LENGTH) & !CLOSED
    }

    /// depth returns the list's count of levels of pages.
    fn depth(&self) -> u64 {
        self.word(DEPTH) & LEVELS
    }

    /// reach returns the most entries the list ever held, below which its pages exist.
    fn reach(&self) -> u64 {
        self.word(DEPTH) >> REACH
    }

    /// counted stores the list's length, its reach growing with it.
    fn counted(&self, length: u64) {
        self.set_word(DEPTH, self.reach().max(length) << REACH | self.depth());
        self.set_word(LENGTH, length);
    }

    /// wants returns how many pages the entry past the list's length needs: none below its reach.
    fn wants(&self) -> usize {
        let length = self.len();
        if length < self.reach() { 0 } else { wants(length, self.depth()) }
    }

    /// open refuses a store to a closed list and returns the list's length.
    fn open(&self) -> Result<u64, Error> {
        if self.word(LENGTH) & CLOSED != 0 {
            return Err(internal("a closed list takes no change"));
        }
        Ok(self.len())
    }

    /// entry returns the entry at an index, refusing one past the list's length.
    fn entry<'h>(&self, hold: &'h Hold<'_>, index: u64) -> Result<Entry<'h>, Error> {
        if index >= self.len() {
            return Err(internal("an index of a list is past its length"));
        }
        self.at(hold, index)
    }

    /// span opens the entries from an index up to another, which the tree holds, so that a change opens every entry it
    /// stores before its first store.
    fn span<'h>(&self, hold: &'h Hold<'_>, from: u64, to: u64) -> Result<Vec<Entry<'h>>, Error> {
        let mut entries = Vec::new();
        entries.try_reserve(to.saturating_sub(from) as usize).map_err(|_| Error::out_of_memory())?;
        for index in from..to {
            entries.push(self.at(hold, index)?);
        }
        Ok(entries)
    }

    /// at returns the entry at an index the tree holds.
    fn at<'h>(&self, hold: &'h Hold<'_>, index: u64) -> Result<Entry<'h>, Error> {
        if index < INLINE {
            return Ok(self.inline(index));
        }
        let entry = index - INLINE;
        let mut page = hold.page(Page::named(self.word(ROOT)))?;
        for level in (2..=self.depth()).rev() {
            page = hold.page(Page::named(page.word(digit(entry, level))?.load()))?;
        }
        Ok(Entry { at: At::Paged(page.word(entry % FANOUT)?) })
    }
}

/// ListView is a list read under its record's guard.
pub struct ListView<'g, S, const I: usize> {
    list: List<S, I>,
    guard: PhantomData<&'g ()>,
}

impl<S: ListAt<I>, const I: usize> ListView<'_, S, I> {
    /// len returns the list's count of entries.
    pub fn len(&self) -> u64 {
        self.list.len()
    }

    /// is_empty reports whether the list has no entry.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// wants returns how many list pages the next `push` or `insert` can need, which a writer allocates before it
    /// takes the lock exclusively.
    pub fn wants(&self) -> usize {
        self.list.wants()
    }

    /// reserving returns how many list pages a `reserve` of room for a number of entries needs.
    pub fn reserving(&self, entries: u64) -> usize {
        let mut depth = self.list.depth();
        let mut wanted = 0;
        for index in self.list.reach()..entries {
            wanted += wants(index, depth);
            if index >= INLINE && (depth == 0 || index - INLINE == capacity(depth)) {
                depth += 1;
            }
        }
        wanted
    }

    /// entry returns the entry at an index, refusing one past the list's length, for no longer than the view.
    pub fn entry<'a>(&'a self, hold: &'a Hold<'_>, index: u64) -> Result<Entry<'a>, Error> {
        self.list.entry(hold, index)
    }

    /// get loads the entry at an index, refusing one past the list's length.
    pub fn get(&self, hold: &Hold<'_>, index: u64) -> Result<u64, Error> {
        Ok(self.list.entry(hold, index)?.load())
    }
}

/// ListMut is a list read and stored under its record's guard held exclusively, which it keeps for its links.
pub struct ListMut<'g, 'a, S, L, const I: usize> {
    list: List<S, I>,
    guard: &'g mut Guard<'a, S, L, Exclusive>,
}

impl<S: ListAt<I>, L: Lock<S>, const I: usize> ListMut<'_, '_, S, L, I> {
    /// view returns the list to read.
    pub fn view(&self) -> ListView<'_, S, I> {
        ListView { list: self.list, guard: PhantomData }
    }

    /// set stores a value over the entry at an index below the list's length.
    pub fn set(&mut self, hold: &Hold<'_>, index: u64, value: u64) -> Result<(), Error> {
        self.list.open()?;
        self.list.entry(hold, index)?.store(value);
        Ok(())
    }

    /// slot makes room for the entry past the last, growing the tree with the pages it is given where the list's
    /// reach ends (a writer gives at least `wants` of them, and keeps what isn't used), and returns its index.
    fn slot(&mut self, hold: &Hold<'_>, spare: &mut Vec<Owned<'_>>) -> Result<u64, Error> {
        let length = self.list.open()?;
        if spare.len() < self.list.wants() {
            return Err(internal("a list's growth wasn't given the pages it wants"));
        }
        if length >= INLINE && length >= self.list.reach() {
            self.grow(hold, length - INLINE, self.list.depth(), spare)?;
        }
        Ok(length)
    }

    /// push adds a value past the last entry: it stores the value and then counts it.
    pub fn push(&mut self, hold: &Hold<'_>, value: u64, spare: &mut Vec<Owned<'_>>) -> Result<(), Error> {
        let length = self.slot(hold, spare)?;
        self.list.at(hold, length)?.store(value);
        self.list.counted(length + 1);
        Ok(())
    }

    /// grow links the pages that the entry past the inline ones at an index needs: a first root, a new root whose
    /// first word names the old one when the tree is full, and a new page at each level below the root whose first
    /// entry it is.
    fn grow(&mut self, hold: &Hold<'_>, entry: u64, depth: u64, spare: &mut Vec<Owned<'_>>) -> Result<(), Error> {
        let wanted = || internal("a list's growth wasn't given the pages it wants");
        let root = self.list.inline(ROOT);
        let depth = if depth == 0 {
            spare.pop().ok_or_else(wanted)?.link(self.guard, &root);
            self.list.set_word(DEPTH, self.list.reach() << REACH | 1);
            return Ok(());
        } else if entry == capacity(depth) {
            let mut new = spare.pop().ok_or_else(wanted)?;
            new.store(0, self.list.word(ROOT))?;
            new.link(self.guard, &root);
            self.list.set_word(DEPTH, self.list.reach() << REACH | (depth + 1));
            depth + 1
        } else {
            depth
        };
        let mut page = hold.page(Page::named(self.list.word(ROOT)))?;
        for level in (2..=depth).rev() {
            let word = page.word(digit(entry, level))?;
            if entry.is_multiple_of(capacity(level - 1)) {
                let child = spare.pop().ok_or_else(wanted)?;
                child.link(self.guard, &Entry { at: At::Paged(page.word(digit(entry, level))?) });
            }
            page = hold.page(Page::named(word.load()))?;
        }
        Ok(())
    }

    /// reserve grows the list's tree with the pages it is given, `reserving` of them, so that it holds a number of
    /// entries with none added: its reach rises to them, so a later `push` or `insert` up to that many takes no page.
    pub fn reserve(&mut self, hold: &Hold<'_>, entries: u64, spare: &mut Vec<Owned<'_>>) -> Result<(), Error> {
        self.list.open()?;
        if entries > MOST {
            return Err(internal("a list holds no more entries than the largest delta's"));
        }
        if spare.len() < self.view().reserving(entries) {
            return Err(internal("a list's growth wasn't given the pages it wants"));
        }
        for index in self.list.reach()..entries {
            if index >= INLINE {
                self.grow(hold, index - INLINE, self.list.depth(), spare)?;
            }
            self.list.set_word(DEPTH, (index + 1) << REACH | self.list.depth());
        }
        Ok(())
    }

    /// insert puts a value at an index, moving the entries from it on up by one, and then counts it, opening every
    /// entry it stores before its first store; only a list that `Removes` takes it.
    pub fn insert(&mut self, hold: &Hold<'_>, index: u64, value: u64, spare: &mut Vec<Owned<'_>>) -> Result<(), Error>
    where
        S: Removes<I>,
    {
        if index > self.list.len() {
            return Err(internal("an index of a list is past its length"));
        }
        let length = self.slot(hold, spare)?;
        let entries = self.list.span(hold, index, length + 1)?;
        for at in (1..entries.len()).rev() {
            entries[at].store(entries[at - 1].load());
        }
        entries[0].store(value);
        self.list.counted(length + 1);
        Ok(())
    }

    /// replace_with puts two values in the place of the entry at an index, the first at the index and the second
    /// after it, moving the later entries up by one, and then counts the one more, opening every entry it stores
    /// before its first store; only a list that `Removes` takes it.
    pub fn replace_with(
        &mut self,
        hold: &Hold<'_>,
        index: u64,
        values: [u64; 2],
        spare: &mut Vec<Owned<'_>>,
    ) -> Result<(), Error>
    where
        S: Removes<I>,
    {
        if index >= self.list.len() {
            return Err(internal("an index of a list is past its length"));
        }
        let length = self.slot(hold, spare)?;
        let entries = self.list.span(hold, index, length + 1)?;
        for at in (2..entries.len()).rev() {
            entries[at].store(entries[at - 1].load());
        }
        entries[1].store(values[1]);
        entries[0].store(values[0]);
        self.list.counted(length + 1);
        Ok(())
    }

    /// remove takes the entry at an index out, moving the entries past it down by one, opening every entry it stores
    /// before its first store; only a list that `Removes` takes it.
    pub fn remove(&mut self, hold: &Hold<'_>, index: u64) -> Result<(), Error>
    where
        S: Removes<I>,
    {
        let length = self.list.open()?;
        if index >= length {
            return Err(internal("an index of a list is past its length"));
        }
        let entries = self.list.span(hold, index, length)?;
        for at in 1..entries.len() {
            entries[at - 1].store(entries[at].load());
        }
        self.list.counted(length - 1);
        Ok(())
    }

    /// close closes the list as the last store it takes, after which every store refuses it and its record's
    /// retirement reads its pages with no lock.
    pub fn close(self) -> Closed<S, I> {
        self.list.set_word(LENGTH, self.list.word(LENGTH) | CLOSED);
        Closed { list: self.list }
    }
}

/// Closed is a closed list, which no store changes, so its pages are read with no lock.
pub struct Closed<S, const I: usize> {
    list: List<S, I>,
}

impl<S: ListAt<I>, const I: usize> Closed<S, I> {
    /// pages returns the names of the list's pages, level by level from its root, for its record's one retirement
    /// call; it opens pages under the hold and takes no lock.
    pub fn pages<'h>(&self, hold: &'h Hold<'h>) -> Pages<'h> {
        let depth = self.list.depth();
        Pages {
            hold,
            root: self.list.word(ROOT),
            depth,
            entries: self.list.reach().saturating_sub(INLINE),
            level: depth,
            next: 0,
        }
    }
}

/// Pages walks a closed list's pages: its root, then each level's pages in order, down to its leaves.
pub struct Pages<'h> {
    hold: &'h Hold<'h>,
    root: u64,
    depth: u64,
    /// entries is the list's count of entries past the inline ones.
    entries: u64,
    /// level is the level of the next page to name, zero when every page is named.
    level: u64,
    /// next is the next page's index among its level's pages.
    next: u64,
}

impl Pages<'_> {
    /// on_level returns the number of the list's pages at a level.
    fn on_level(&self, level: u64) -> u64 {
        if level == self.depth { 1 } else { self.entries.div_ceil(capacity(level)) }
    }

    /// name returns the name of a page at a level, found from the root by its first entry's path.
    fn name(&self, level: u64, index: u64) -> Result<Page, Error> {
        let entry = index * capacity(level);
        let mut name = self.root;
        for above in (level + 1..=self.depth).rev() {
            name = self.hold.page(Page::named(name))?.word(digit(entry, above))?.load();
        }
        Ok(Page::named(name))
    }
}

impl Iterator for Pages<'_> {
    type Item = Result<Page, Error>;

    fn next(&mut self) -> Option<Result<Page, Error>> {
        if self.level == 0 {
            return None;
        }
        let named = self.name(self.level, self.next);
        self.next += 1;
        if self.next == self.on_level(self.level) {
            self.level -= 1;
            self.next = 0;
        }
        if named.is_err() {
            self.level = 0;
        }
        Some(named)
    }
}
