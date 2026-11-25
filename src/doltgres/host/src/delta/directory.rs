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

//! The delta's page directory as a host keeps it (design item 14's S6): what each host gives of its slots, its
//! header, its bodies, and the epochs, and the sequences over them, an allocation, an open, an append, a seal, a
//! link, a retirement's batch, and the free pass, written once so the server's host and the simulation's can't differ
//! in them.

use super::Leaving;
use super::blocks::{self, Block, Region};
use super::page::{self, BLOCK, Page, SLOT_BYTES, Size, State};
use crate::Error;

/// Field is one of a slot's words beside its generation-and-state word.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Field {
    /// Body is the place of the page's body in its host's memory, or zero while the slot has none.
    Body,
    /// Sizes is the capacity of the page's bytes in its lower half and the count of its words in its upper half.
    Sizes,
    /// Mark is the page's filled mark, which only grows.
    Mark,
    /// Owner is the location of the record whose lock may append to the page.
    Owner,
    /// Lock is one more than that lock's index among its record's locks, or zero for a page no lock may append to.
    Lock,
    /// Stamp is the epoch of the page's retirement.
    Stamp,
    /// Shape is the id of the shape of the record whose lock may append to the page.
    Shape,
}

/// Point is a place inside a sequence where the fork's tests hold a process.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Point {
    /// Opening is between an open's first load of a slot's word and its loads of the slot's other words.
    Opening,
    /// Stamped is between a retirement's stores and its advance of the epoch.
    Stamped,
    /// Claimed is between a free pass's claim of a slot and its give-back of the body.
    Claimed,
    /// Passing is the start of a free pass.
    Passing,
    /// Walked is between a free pass's last walk of records and its walk of pages.
    Walked,
    /// Stamping is between a retirement's store of a record's or a page's stamp and its store of the retired state.
    Stamping,
    /// Oldest is between a free pass's load of the oldest epoch and its walks.
    Oldest,
    /// Waiting is between a refused allocation's answer and its count among the budget's waiters.
    Waiting,
    /// Hinting is between a free pass's store of a slot's free word and its set of the slot's bit of the free hint.
    Hinting,
}

/// Owner is the lock that may append to a page: its record's shape's id, its record's location, and its index
/// among the record's locks. A shape has one form and its locations are its own, so no two locks share an owner.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Owner {
    /// shape is the id of the shape of the lock's record.
    pub shape: u64,
    /// location is the location of the lock's record.
    pub location: u64,
    /// lock is the lock's index among its record's locks.
    pub lock: u64,
}

/// Opened is a page an open reached: its slot, its body's place, and its size.
#[derive(Debug, Clone, Copy)]
pub struct Opened {
    /// slot is the page's slot in the directory.
    pub slot: u32,
    /// body is the place of the page's body.
    pub body: u64,
    /// size is the page's size.
    pub size: Size,
}

/// Directory is what a host gives of its page directory and its region of blocks. Nothing in it fails but the
/// allocation lock's take, and nothing in it allocates the caller's memory.
pub trait Directory {
    /// Raise is what the host's allocation lock raises with.
    type Raise;

    /// slots returns the directory's count of slots.
    fn slots(&self) -> u32;

    /// budget returns the delta's memory in bytes.
    fn budget(&self) -> u64;

    /// word loads a slot's generation-and-state word.
    fn word(&self, slot: u32) -> u64;

    /// swap stores a slot's generation-and-state word over the one loaded, reporting whether it was still there.
    fn swap(&self, slot: u32, loaded: u64, word: u64) -> bool;

    /// load loads one of a slot's other words.
    fn load(&self, slot: u32, field: Field) -> u64;

    /// store stores one of a slot's other words.
    fn store(&self, slot: u32, field: Field, value: u64);

    /// blocks returns the count of the region's blocks.
    fn blocks(&self) -> u32;

    /// region loads one of the region's words.
    fn region(&self, word: Region) -> u64;

    /// set_region stores one of the region's words.
    fn set_region(&self, word: Region, value: u64);

    /// block loads one of a block's words.
    fn block(&self, block: u32, word: Block) -> u64;

    /// set_block stores one of a block's words.
    fn set_block(&self, block: u32, word: Block, value: u64);

    /// lock_region takes the region's lock, which can't fail and is held only for a block's take or give.
    fn lock_region(&self);

    /// unlock_region releases the region's lock.
    fn unlock_region(&self);

    /// frees returns the count of frees.
    fn frees(&self) -> u64;

    /// freed adds one to the count of frees and wakes the budget's waiters.
    fn freed(&self);

    /// left loads the count of retirements since the last free pass that left no retired slot unfreed.
    fn left(&self) -> u64;

    /// leave adds one to the count of retirements a free pass hasn't found all freed.
    fn leave(&self);

    /// settle stores zero over the count of retirements if it still holds the count a free pass loaded.
    fn settle(&self, loaded: u64);

    /// kept loads the lowest stamp of a retirement a free pass found and couldn't free, or zero for none, since every
    /// stamp is an epoch and epochs start at one (bugs row 124, row 111's give-back text's item 1).
    fn kept(&self) -> u64;

    /// kept_swap stores the kept stamp over the one loaded, reporting whether it was still there.
    fn kept_swap(&self, loaded: u64, word: u64) -> bool;

    /// waiting loads the count of processes asleep at the budget, or about to sleep.
    fn waiting(&self) -> u64;

    /// count_waiter adds one to the count of the budget's waiters, or takes one away.
    fn count_waiter(&self, add: bool);

    /// overshoot loads the overshoot word: the slot plus one of the page whose replacement took a page past the
    /// budget in its low half, and its copy's slot plus one in its high half once the copy is taken, or zero while
    /// none has (bugs row 110).
    fn overshoot(&self) -> u64;

    /// overshoot_swap stores the overshoot word over the one loaded, reporting whether it was still there.
    fn overshoot_swap(&self, loaded: u64, word: u64) -> bool;

    /// hand returns the slot a sweep for a free slot starts at, and moves it on.
    fn hand(&self) -> u32;

    /// hint loads a word of the free hint, a bit for each of `HINTED` slots below the block past the budget that is
    /// set once the slot is free and cleared once a claim takes it (bugs row 150).
    fn hint(&self, word: u32) -> u64;

    /// hint_set sets a slot's bit of the free hint.
    fn hint_set(&self, slot: u32);

    /// hint_clear clears a slot's bit of the free hint.
    fn hint_clear(&self, slot: u32);

    /// spent loads the count of the spent slots below the block past the budget, or past it.
    fn spent(&self, over: bool) -> u64;

    /// spend counts a spent slot below the block past the budget, or past it.
    fn spend(&self, over: bool);

    /// lock takes the allocation lock exclusively.
    fn lock(&self, raise: &Self::Raise) -> Result<(), Error>;

    /// unlock releases the allocation lock.
    fn unlock(&self);

    /// zero zeroes the body of a page of a size.
    fn zero(&self, body: u64, size: Size);

    /// write copies bytes into a body at an offset.
    fn write(&self, body: u64, at: u64, bytes: &[u8]);

    /// set stores one of a body's words.
    fn set(&self, body: u64, index: u64, value: u64);

    /// own lists a page the running process owns, in room its caller reserved.
    fn own(&self, name: Page);

    /// disown takes a page off the list of those the running process owns.
    fn disown(&self, name: Page);

    /// retiring takes the epoch lock exclusively and returns the current epoch.
    fn retiring(&self) -> u64;

    /// retiring_now takes the epoch lock exclusively when no process holds it and returns the current epoch, or
    /// nothing while it is held.
    fn retiring_now(&self) -> Option<u64>;

    /// retired advances the epoch and releases the epoch lock.
    fn retired(&self);

    /// unretired releases the epoch lock `retiring` took, with the epoch unchanged.
    fn unretired(&self);

    /// epoch returns the current epoch.
    fn epoch(&self) -> u64;

    /// oldest returns the oldest epoch a live reader started at, or the current epoch when no reader is live.
    fn oldest(&self) -> u64;

    /// stop holds the running process at a point of a sequence when a test asked for it, and can't fail.
    fn stop(&self, point: Point);

    /// aged returns the number of the server's aged kinds, numbered in the table's order.
    fn aged(&self) -> u32;

    /// aged_kind returns the number of the aged kind of a shape's id, or nothing for an id no aged kind has.
    fn aged_kind(&self, id: u64) -> Option<u32>;

    /// records returns the count of an aged kind's records.
    fn records(&self, kind: u32) -> u32;

    /// record_word loads the generation-and-state word of an aged kind's record.
    fn record_word(&self, kind: u32, index: u32) -> u64;

    /// record_swap stores a record's word over the one loaded, reporting whether it was still there.
    fn record_swap(&self, kind: u32, index: u32, loaded: u64, word: u64) -> bool;

    /// stamp loads the epoch of a record's retirement.
    fn stamp(&self, kind: u32, index: u32) -> u64;

    /// set_stamp stores the epoch of a record's retirement.
    fn set_stamp(&self, kind: u32, index: u32, stamp: u64);

    /// record_hand returns the record a sweep of an aged kind's pool starts at, and moves it on.
    fn record_hand(&self, kind: u32) -> u32;

    /// clear zeroes a record's parts for its maker, before anything names it.
    fn clear(&self, kind: u32, index: u32);

    /// made lists a record the running process made, in room its caller reserved.
    fn made(&self, kind: u32, index: u32);

    /// unmade takes a record off the list of those the running process made.
    fn unmade(&self, kind: u32, index: u32);
}

/// internal returns the fork's internal error with a message.
fn internal(message: &str) -> Error {
    Error::fork(b"XX000", message.to_owned())
}

/// sizes returns the word that holds a page's size.
fn sizes(size: Size) -> u64 {
    u64::from(size.words) << 32 | u64::from(size.bytes)
}

/// sized returns the size a slot's word holds.
fn sized(word: u64) -> Size {
    Size { bytes: word as u32, words: (word >> 32) as u32 }
}

/// bytes_at returns where a page's bytes start in its body: after its words.
fn bytes_at(size: Size) -> u64 {
    u64::from(size.words) * size_of::<u64>() as u64
}

/// slot returns the slot a name names, which the name's number always fits.
fn slot(name: Page) -> u32 {
    name.number() as u32
}

/// HINTED is the count of slots each word of the free hint has a bit for.
pub const HINTED: u32 = 64;

/// ordinary returns the count of the directory's slots below the block past the budget's, which the free hint covers.
fn ordinary<D: Directory>(directory: &D) -> u32 {
    directory.slots() - (BLOCK / SLOT_BYTES) as u32
}

/// The block past the budget's slots are whole words of the free hint.
const _: () = assert!(BLOCK / SLOT_BYTES == HINTED as u64);

/// alloc takes a page of each size, all of them or none, listing their names. It answers nothing when it took
/// them, and the count of frees it loaded before it tried when the budget or the slots refused them even after the
/// free pass, which is the budget's signal. Its caller reserved the room for the names, here and in the
/// process's own list.
pub fn alloc<D: Directory>(
    directory: &D,
    raise: &D::Raise,
    wanted: &[Size],
    names: &mut Vec<Page>,
) -> Result<Option<u64>, Error> {
    let budget = directory.budget();
    if wanted.iter().any(|size| !size.allocates(budget)) {
        return Err(internal("a page of the delta has no body, or one no empty delta could give"));
    }
    loop {
        let frees = directory.frees();
        directory.lock(raise)?;
        let taken = take_all(directory, wanted, names);
        directory.unlock();
        if taken? {
            return Ok(None);
        }
        pass(directory);
        if directory.frees() == frees {
            return Ok(Some(frees));
        }
    }
}

/// alloc_replacing takes a page of a size for the copy of a page it replaces, a compaction's: as `alloc`, and, when the
/// budget refuses it, past the budget while no other replacement is, taking the overshoot word for the replaced page
/// and then its copy, which a free of the replaced page, or a release of either unlinked, clears (bugs row 110). The
/// copy is no larger than the page it replaces, so the delta is never more than one page past its budget. It answers as
/// `alloc` does.
pub fn alloc_replacing<D: Directory>(
    directory: &D,
    raise: &D::Raise,
    size: Size,
    replaced: Page,
    names: &mut Vec<Page>,
) -> Result<Option<u64>, Error> {
    let frees = match alloc(directory, raise, &[size], names)? {
        None => return Ok(None),
        Some(frees) => frees,
    };
    let word = u64::from(slot(replaced)) + 1;
    if !directory.overshoot_swap(0, word) {
        return Ok(Some(frees));
    }
    directory.lock(raise)?;
    let taken = take(directory, size, true);
    directory.unlock();
    match taken {
        Ok(Some(name)) => {
            directory.overshoot_swap(word, word | (u64::from(slot(name)) + 1) << 32);
            names.push(name);
            Ok(None)
        }
        refused => {
            directory.overshoot_swap(word, 0);
            refused.map(|_| Some(frees))
        }
    }
}

/// freed clears the overshoot word when a freed slot held the page a replacement past the budget replaced, or,
/// released unlinked, its copy (bugs row 110; orev-18 S1).
fn freed<D: Directory>(directory: &D, slot: u32, unlinked: bool) {
    let over = directory.overshoot();
    let named = u64::from(slot) + 1;
    if over & u64::from(u32::MAX) == named || (unlinked && over >> 32 == named) {
        directory.overshoot_swap(over, 0);
    }
}

/// take_all takes a page of each size under the allocation lock, reporting whether it took them all; when the
/// budget or the slots refuse one, it gives back what it took.
fn take_all<D: Directory>(directory: &D, wanted: &[Size], names: &mut Vec<Page>) -> Result<bool, Error> {
    for &size in wanted {
        match take(directory, size, false) {
            Ok(Some(name)) => names.push(name),
            refused => {
                for name in names.drain(..) {
                    release(directory, name);
                }
                return refused.map(|_| false);
            }
        }
    }
    Ok(true)
}

/// take claims a free slot for a page of a size, lists it as the process's own, and takes its body from the region
/// of blocks, answering nothing when no slot is free or the budget refuses the body, with the slot given back; only a
/// replacement past the budget, `overshooting`, may take the slots and the block the region keeps past the budget.
fn take<D: Directory>(directory: &D, size: Size, overshooting: bool) -> Result<Option<Page>, Error> {
    let Some((slot, word)) = claim(directory, overshooting)? else { return Ok(None) };
    let name = Page::of(slot, word);
    for (field, value) in [(Field::Body, 0), (Field::Sizes, sizes(size)), (Field::Mark, 0), (Field::Lock, 0)] {
        directory.store(slot, field, value);
    }
    directory.own(name);
    match blocks::take(directory, size, overshooting) {
        Some(body) => {
            directory.store(slot, Field::Body, body);
            Ok(Some(name))
        }
        None => {
            release(directory, name);
            Ok(None)
        }
    }
}

/// claim claims a free slot, under the allocation lock, returning it with its word, or nothing when none is free. The
/// last block's worth of slots are the block past the budget's (orev-21 R1): an ordinary claim takes only the slots
/// below them, which hold every page the budget's blocks can, and a replacement past the budget sweeps them first, a
/// word at a time, and then the rest, which hold every copy the block past the budget can, since each replaces a page.
/// The slots below are found by the free hint (bugs row 150): from the word the hand names, around, a set bit's slot
/// is taken by its word's compare-and-swap and then its bit cleared, and a word with no bit set is skipped. A directory
/// whose every slot a claim may take is spent refuses with 53200, since no free or spill would end a wait for it.
fn claim<D: Directory>(directory: &D, overshooting: bool) -> Result<Option<(u32, u64)>, Error> {
    let (slots, ordinary) = (directory.slots(), ordinary(directory));
    let taken = |slot: u32| {
        let loaded = directory.word(slot);
        page::claim(loaded).filter(|&owned| directory.swap(slot, loaded, owned))
    };
    if overshooting && let Some((slot, owned)) = (ordinary..slots).find_map(|slot| Some((slot, taken(slot)?))) {
        return Ok(Some((slot, owned)));
    }
    let (words, start) = (ordinary / HINTED, directory.hand());
    for step in 0..words {
        let word = ((u64::from(start) + u64::from(step)) % u64::from(words)) as u32;
        let mut bits = directory.hint(word);
        while bits != 0 {
            let slot = word * HINTED + bits.trailing_zeros();
            bits &= bits - 1;
            if let Some(owned) = taken(slot) {
                directory.hint_clear(slot);
                return Ok(Some((slot, owned)));
            }
        }
    }
    let past = !overshooting || directory.spent(true) == u64::from(slots - ordinary);
    if directory.spent(false) == u64::from(ordinary) && past {
        return Err(Error::hinted(
            b"53200",
            "out of shared memory",
            "The delta's page directory has spent every slot; restart the server.",
        ));
    }
    Ok(None)
}

/// emptied stores the word of a slot whose page is gone over the one loaded, free at its next generation or spent at
/// its last, and answers it for `hinted`.
fn emptied<D: Directory>(directory: &D, slot: u32, loaded: u64) -> u64 {
    let released = page::released(loaded);
    directory.swap(slot, loaded, released);
    released
}

/// hinted follows a slot's emptied word: a free slot below the block past the budget gets its bit of the free hint,
/// after the word, and a spent one is counted spent and never in the free hint again (bugs row 150, H1). Its caller
/// counts any free after, so a waiter woken by the count finds the bit set (H3).
fn hinted<D: Directory>(directory: &D, slot: u32, released: u64) {
    let past = slot >= ordinary(directory);
    match page::state(released) {
        State::Spent => directory.spend(past),
        _ if !past => directory.hint_set(slot),
        _ => {}
    }
}

/// release gives an owned page's body back to its block, frees its slot at the next generation, and takes it off
/// the process's list.
fn release<D: Directory>(directory: &D, name: Page) {
    let slot = slot(name);
    let body = directory.load(slot, Field::Body);
    if body != 0 {
        blocks::give(directory, body);
        directory.store(slot, Field::Body, 0);
    }
    let released = emptied(directory, slot, directory.word(slot));
    hinted(directory, slot, released);
    freed(directory, slot, true);
    directory.disown(name);
}

/// give_back gives back a page its owner dropped unlinked, or left at its exit, which counts as a free.
pub fn give_back<D: Directory>(directory: &D, name: Page) {
    release(directory, name);
    directory.freed();
}

/// fill copies a record above a page's filled mark and then advances the mark, returning the record's offset, or
/// refuses a record that doesn't fit, changing nothing.
fn fill<D: Directory>(directory: &D, slot: u32, body: u64, size: Size, record: &[u8]) -> Result<u32, Error> {
    let mark = directory.load(slot, Field::Mark);
    let filled = page::appended(mark, record.len() as u64, u64::from(size.bytes))
        .ok_or_else(|| internal("a record doesn't fit the page it is appended to"))?;
    directory.write(body, bytes_at(size) + mark, record);
    directory.store(slot, Field::Mark, filled);
    Ok(mark as u32)
}

/// owned_append appends a record to a page its owner hasn't linked, returning the record's offset.
pub fn owned_append<D: Directory>(directory: &D, name: Page, record: &[u8]) -> Result<u32, Error> {
    let slot = slot(name);
    let size = sized(directory.load(slot, Field::Sizes));
    fill(directory, slot, directory.load(slot, Field::Body), size, record)
}

/// owned_store stores a word of a page its owner hasn't linked, refusing an index past the page's words.
pub fn owned_store<D: Directory>(directory: &D, name: Page, index: u64, value: u64) -> Result<(), Error> {
    let slot = slot(name);
    worded(sized(directory.load(slot, Field::Sizes)), index)?;
    directory.set(directory.load(slot, Field::Body), index, value);
    Ok(())
}

/// make claims a free record of an aged kind for the running process and clears its parts, sweeping the kind's
/// pool once from its hand; when none is free it runs the free pass and sweeps again if the count of frees moved.
/// It answers the record's index and location, or, when none was free, the count of frees it loaded before it
/// looked, which is the budget's signal. A pool whose every slot is spent is refused, since no free would end a wait
/// for it. Its caller reserved the room in the process's list.
pub fn make<D: Directory>(directory: &D, kind: u32, name: &str) -> Result<Result<(u32, u64), u64>, Error> {
    loop {
        let frees = directory.frees();
        if let Some((index, word)) = claim_record(directory, kind, name)? {
            directory.clear(kind, index);
            directory.made(kind, index);
            return Ok(Ok((index, Page::of(index, word).value())));
        }
        pass(directory);
        if directory.frees() == frees {
            return Ok(Err(frees));
        }
    }
}

/// claim_record sweeps an aged kind's pool once from its hand and claims the first free record, returning it with
/// its word, or nothing when none is free.
fn claim_record<D: Directory>(directory: &D, kind: u32, name: &str) -> Result<Option<(u32, u64)>, Error> {
    let (count, start) = (directory.records(kind), directory.record_hand(kind));
    let mut spent = 0;
    for step in 0..count {
        let index = ((u64::from(start) + u64::from(step)) % u64::from(count)) as u32;
        let loaded = directory.record_word(kind, index);
        match page::claim(loaded) {
            Some(owned) if directory.record_swap(kind, index, loaded, owned) => return Ok(Some((index, owned))),
            Some(_) => {}
            None => spent += u32::from(page::state(loaded) == State::Spent),
        }
    }
    if spent == count {
        return Err(internal(&format!("every slot of the delta's {name} records is spent; restart the server")));
    }
    Ok(None)
}

/// unmake gives back a record its maker dropped unlinked, or left at its exit, which counts as a free.
pub fn unmake<D: Directory>(directory: &D, kind: u32, index: u32) {
    let word = directory.record_word(kind, index);
    directory.record_swap(kind, index, word, page::released(word));
    directory.unmade(kind, index);
    directory.freed();
}

/// publish makes a record its maker owns published, off the process's list.
pub fn publish<D: Directory>(directory: &D, kind: u32, index: u32) {
    let word = directory.record_word(kind, index);
    directory.record_swap(kind, index, word, page::link(word));
    directory.unmade(kind, index);
}

/// open_record reaches the record of an aged kind a location names under a hold begun at an epoch, as `open`
/// reaches a page: it loads the record's word, then its stamp, then its word again, and refuses an index past the
/// pool and whatever `page::admits` refuses of those loads. It returns the record's index.
pub fn open_record<D: Directory>(directory: &D, kind: u32, location: u64, epoch: u64) -> Result<u32, Error> {
    let name = Page::named(location);
    if name.number() >= u64::from(directory.records(kind)) {
        return Err(internal("a location of the delta names no record its hold reaches"));
    }
    let index = name.number() as u32;
    let before = directory.record_word(kind, index);
    directory.stop(Point::Opening);
    let stamp = directory.stamp(kind, index);
    let after = directory.record_word(kind, index);
    if !page::admits(page::seen(before), stamp, page::seen(after), name.generation(), epoch) {
        return Err(internal("a location of the delta names no record its hold reaches"));
    }
    Ok(index)
}

/// link records the lock that may append to an owned page and makes the page live, off the process's list.
pub fn link<D: Directory>(directory: &D, name: Page, owner: Owner) {
    let slot = slot(name);
    directory.store(slot, Field::Shape, owner.shape);
    directory.store(slot, Field::Owner, owner.location);
    directory.store(slot, Field::Lock, owner.lock + 1);
    let word = directory.word(slot);
    directory.swap(slot, word, page::link(word));
    directory.disown(name);
}

/// open reaches the page a name names under a hold begun at an epoch: it loads the slot's word, then its stamp and
/// its other words, then its word again, and refuses a number past the directory and whatever `page::admits`
/// refuses of those loads.
pub fn open<D: Directory>(directory: &D, name: Page, epoch: u64) -> Result<Opened, Error> {
    let refused = || internal("a name of the delta names no page its hold reaches");
    if name.number() >= u64::from(directory.slots()) {
        return Err(refused());
    }
    let slot = slot(name);
    let before = directory.word(slot);
    directory.stop(Point::Opening);
    let stamp = directory.load(slot, Field::Stamp);
    let body = directory.load(slot, Field::Body);
    let size = sized(directory.load(slot, Field::Sizes));
    let after = directory.word(slot);
    if !page::admits(page::seen(before), stamp, page::seen(after), name.generation(), epoch) {
        return Err(refused());
    }
    Ok(Opened { slot, body, size })
}

/// readable checks a read of a length at an offset against a page's filled mark, loaded now, and returns where
/// the read starts in the page's body.
pub fn readable<D: Directory>(directory: &D, opened: &Opened, offset: u64, len: u64) -> Result<u64, Error> {
    if !page::lends(directory.load(opened.slot, Field::Mark), offset, len) {
        return Err(internal("a read of a page of the delta ends past its filled mark"));
    }
    Ok(bytes_at(opened.size) + offset)
}

/// room returns how many bytes an opened page takes above its filled mark, which never passes its capacity, and none
/// for a sealed page, which takes no append.
pub fn room<D: Directory>(directory: &D, opened: &Opened) -> u64 {
    if directory.load(opened.slot, Field::Lock) == 0 {
        return 0;
    }
    u64::from(opened.size.bytes) - directory.load(opened.slot, Field::Mark)
}

/// filled returns how many bytes an opened page holds below its filled mark.
pub fn filled<D: Directory>(directory: &D, opened: &Opened) -> u64 {
    directory.load(opened.slot, Field::Mark)
}

/// worded checks a word's index against a page's count of words.
pub fn worded(size: Size, index: u64) -> Result<(), Error> {
    if index >= u64::from(size.words) {
        return Err(internal("a word of a page of the delta is past the page's words"));
    }
    Ok(())
}

/// owns checks that a lock is the one a page's slot records, which no lock is once the page is sealed.
fn owns<D: Directory>(directory: &D, slot: u32, owner: Owner) -> Result<(), Error> {
    let recorded =
        (directory.load(slot, Field::Shape), directory.load(slot, Field::Owner), directory.load(slot, Field::Lock));
    if recorded != (owner.shape, owner.location, owner.lock + 1) {
        return Err(internal("a page of the delta takes an append or a seal only under the lock that owns it"));
    }
    Ok(())
}

/// append appends a record to an opened page under the exclusive guard of the lock its slot records, returning the
/// record's offset.
pub fn append<D: Directory>(directory: &D, opened: &Opened, owner: Owner, record: &[u8]) -> Result<u32, Error> {
    owns(directory, opened.slot, owner)?;
    fill(directory, opened.slot, opened.body, opened.size, record)
}

/// seal clears an opened page's owner under the exclusive guard of the lock its slot records, after which no
/// append to it is taken. A seal of a page already sealed does nothing, since a split that fails after its seal
/// leaves its range's own pages sealed and the next split of the range seals them again; a seal of an unsealed page
/// under any guard but its owner's is still refused.
pub fn seal<D: Directory>(directory: &D, opened: &Opened, owner: Owner) -> Result<(), Error> {
    if directory.load(opened.slot, Field::Lock) == 0 {
        return Ok(());
    }
    owns(directory, opened.slot, owner)?;
    directory.store(opened.slot, Field::Lock, 0);
    Ok(())
}

/// retire retires one batch of records and names under one take of the epoch lock: it stores the batch's one stamp
/// and then the retired state in each record that is still published at its location and each page that is still
/// live at its name, and then advances the epoch.
pub fn retire<D: Directory>(directory: &D, records: &[Leaving], names: &[Page]) {
    let stamp = directory.retiring();
    for leaving in records {
        let Some(kind) = directory.aged_kind(leaving.id) else { continue };
        let name = Page::named(leaving.location);
        if name.number() >= u64::from(directory.records(kind)) {
            continue;
        }
        let index = name.number() as u32;
        let loaded = directory.record_word(kind, index);
        if let Some(retired) = page::retire(loaded, name) {
            directory.set_stamp(kind, index, stamp);
            directory.stop(Point::Stamping);
            directory.record_swap(kind, index, loaded, retired);
        }
    }
    for &name in names {
        if name.number() >= u64::from(directory.slots()) {
            continue;
        }
        let slot = slot(name);
        let loaded = directory.word(slot);
        if let Some(retired) = page::retire(loaded, name) {
            directory.store(slot, Field::Stamp, stamp);
            directory.stop(Point::Stamping);
            directory.swap(slot, loaded, retired);
        }
    }
    directory.stop(Point::Stamped);
    directory.leave();
    lowered(directory, stamp);
    directory.retired();
}

/// pass is the free pass: with the oldest published epoch loaded once, it walks every aged kind's records in the
/// table's order and then the page directory, and claims each retired record or slot whose stamp that epoch has
/// passed and frees it at its next generation, a page's body given back to its block first, each of which counts as
/// a free. A pass that loses a claim does nothing with that record or slot. A pass that left no retired record or
/// slot unfreed clears the count of retirements it loaded, unless a retirement since changed it. It loads the current
/// epoch before the oldest, and records the lowest stamp it kept (`held`).
pub fn pass<D: Directory>(directory: &D) {
    directory.stop(Point::Passing);
    let left = directory.left();
    let mut kept = None;
    let current = directory.epoch();
    let oldest = directory.oldest();
    directory.stop(Point::Oldest);
    for kind in 0..directory.aged() {
        for index in 0..directory.records(kind) {
            let loaded = directory.record_word(kind, index);
            let Some(freeing) = page::reap(loaded) else { continue };
            let stamp = directory.stamp(kind, index);
            if !page::passed(stamp, oldest) {
                kept = Some(kept.map_or(stamp, |kept: u64| kept.min(stamp)));
                continue;
            }
            if !directory.record_swap(kind, index, loaded, freeing) {
                continue;
            }
            directory.record_swap(kind, index, freeing, page::released(freeing));
            directory.freed();
        }
    }
    directory.stop(Point::Walked);
    for slot in 0..directory.slots() {
        let loaded = directory.word(slot);
        let Some(freeing) = page::reap(loaded) else { continue };
        let stamp = directory.load(slot, Field::Stamp);
        if !page::passed(stamp, oldest) {
            kept = Some(kept.map_or(stamp, |kept: u64| kept.min(stamp)));
            continue;
        }
        if !directory.swap(slot, loaded, freeing) {
            continue;
        }
        directory.stop(Point::Claimed);
        blocks::give(directory, directory.load(slot, Field::Body));
        directory.store(slot, Field::Body, 0);
        freed(directory, slot, false);
        let released = emptied(directory, slot, freeing);
        directory.stop(Point::Hinting);
        hinted(directory, slot, released);
        directory.freed();
    }
    if kept.is_none() {
        directory.settle(left);
    }
    held(directory, (current, oldest), kept.unwrap_or(0));
}

/// rank orders kept stamps, zero, for none, after every stamp.
fn rank(stamp: u64) -> u64 {
    if stamp == 0 { u64::MAX } else { stamp }
}

/// lowered lowers the kept stamp to a stamp below it, by a compare-exchange retried while the stored stamp is higher,
/// and answers whether it was above it: for a pass that kept a retirement, and for each retirement under the epoch
/// lock, since a retirement is unfreed when it is stamped, so a hold begun at or before it wakes the budget's waiters
/// at its end, whatever pass runs first (PerfPF 11's first lost wake).
fn lowered<D: Directory>(directory: &D, stamp: u64) -> bool {
    loop {
        let stored = directory.kept();
        if rank(stamp) >= rank(stored) {
            return false;
        }
        if directory.kept_swap(stored, stamp) {
            return true;
        }
    }
}

/// EVERY is a kept stamp past every epoch, so every hold's end wakes the budget's waiters, until a pass or a
/// retirement lowers it (bugs row 160).
const EVERY: u64 = u64::MAX - 1;

/// held records the lowest stamp a free pass kept, zero for none, as the kept stamp, which decides only which hold's
/// end wakes the budget's waiters (`released`) and never a free, since a pass frees a retirement only once the oldest
/// epoch has passed its own stamp (bugs row 124, row 111's give-back text's item 1). A lower one lowers it at once
/// (`lowered`). A higher one raises it (`raise`), since every retirement the walk missed is stamped at or after the
/// epoch the pass loaded before the oldest, so none unfreed is below the pass's lowest, and a stamp left below that
/// keeps the end of the hold that lets one be freed from waking the waiters (bugs row 160). It raises only under the
/// epoch lock held exclusively, which it doesn't wait for: while a retirement holds it between its stores, the pass
/// leaves the stamp to that retirement's own pass. None clears it only while the epoch is still the one the pass
/// loaded, since then no retirement began since and every one unfreed is in the walk; otherwise it raises it to
/// `EVERY`. A hold that ended between the pass's load of the oldest epoch and its raise read the stamp before it, so
/// the pass reloads the oldest after a raise and, when a reader ended since, counts the wake that hold's end didn't
/// (PerfPF 11's second lost wake).
fn held<D: Directory>(directory: &D, (current, oldest): (u64, u64), kept: u64) {
    if lowered(directory, kept) {
        return;
    }
    let Some(epoch) = directory.retiring_now() else { return };
    let raised = match (kept, epoch == current) {
        (0, true) => {
            let stored = directory.kept();
            stored != 0 && directory.kept_swap(stored, 0)
        }
        (0, false) => raise(directory, EVERY),
        (kept, _) => raise(directory, kept),
    };
    directory.unretired();
    if raised && directory.oldest() > oldest {
        directory.freed();
    }
}

/// raise raises the kept stamp to a stamp above it, by a compare-exchange retried while the stored stamp is lower,
/// and answers whether it was below it.
fn raise<D: Directory>(directory: &D, stamp: u64) -> bool {
    loop {
        let stored = directory.kept();
        if rank(stamp) <= rank(stored) {
            return false;
        }
        if directory.kept_swap(stored, stamp) {
            return true;
        }
    }
}

/// released wakes the budget's waiters at the end of a hold or a reader begun at an epoch, its epoch already
/// released, while one
/// waits and a retirement may be unfreed (bugs row 72): a retirement under a hold, the retirer's own or an older
/// reader's, can't be freed by its own pass, and a waiter wakes only when the count of frees moves, so the end of a
/// hold that may have kept one counts a wake as a free, and the waiter's allocation then runs the free pass that frees
/// what the released epoch let go. Only a hold begun at or before the lowest stamp a pass kept may have kept one, since
/// the reader that keeps that retirement keeps every later one too (bugs row 124); a later hold's end counts nothing,
/// so no waiter wakes to a pass that frees nothing. It allocates nothing and waits on nothing, since a hold ends in a
/// drop.
pub fn released<D: Directory>(directory: &D, epoch: u64) {
    let kept = directory.kept();
    if directory.waiting() > 0 && directory.left() != 0 && kept != 0 && epoch <= kept {
        directory.freed();
    }
}

/// waiting counts a process refused at the budget among its waiters before it sleeps, and then runs the free pass
/// while a retirement may be unfreed, so a hold that ended before the count, reading no waiter, leaves the waiter no
/// page its release let go; `woken` takes the count back after the sleep.
pub fn waiting<D: Directory>(directory: &D) {
    directory.stop(Point::Waiting);
    directory.count_waiter(true);
    if directory.left() != 0 {
        pass(directory);
    }
}

/// woken takes a process off the budget's count of waiters after its sleep.
pub fn woken<D: Directory>(directory: &D) {
    directory.count_waiter(false);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A slot's size word holds both of a size's numbers, and a page's bytes start after its words.
    #[test]
    fn a_size_fits_its_word() {
        let size = Size { bytes: 128 << 10, words: u32::MAX };
        assert_eq!(sized(sizes(size)), size);
        assert_eq!(bytes_at(Size { bytes: 4096, words: 3 }), 24);
        assert_eq!(bytes_at(Size { bytes: 4096, words: 0 }), 0);
    }

    /// A word's index is checked against its page's count of words.
    #[test]
    fn a_words_index_is_below_its_pages_count() {
        let size = Size { bytes: 0, words: 2 };
        assert!(worded(size, 1).is_ok());
        assert_eq!(worded(size, 2).map_err(|error| *error.sqlstate()), Err(*b"XX000"));
    }
}
