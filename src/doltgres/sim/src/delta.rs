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

//! The simulation's page directory (design item 14's S6): the delta's slots, header, and bodies in the test
//! process's memory, under `host`'s own sequences, so a case that passes here passes the server's host's steps. A
//! body is its bytes and its words in vectors by its place in the region of blocks, lent by copy. The doors that the
//! design names yield, and the budget's wait is a wait the scheduler ends when a free is counted.

use std::collections::BTreeMap;
use std::ffi::CStr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::ThreadId;

use host::Error;
use host::cache::Bytes;
use host::delta::blocks::{self, Block, Region};
use host::delta::directory::{self, Directory, Field, Opened, Owner, Point};
use host::delta::{Leaving, Page, Size, WordOp, page};
use host::shared::Kind;

use crate::run::{self, Part};
use crate::shared;

/// DEFAULT is the delta's memory until a test sizes it: the setting's default.
const DEFAULT: u64 = 32 << 20;

/// Slot is a slot of the directory: its generation-and-state word and its other words, in `at`'s order.
#[derive(Clone, Copy, Default)]
struct Slot {
    word: u64,
    fields: [u64; 7],
}

/// Body is a page's body: its bytes and its words.
struct Body {
    bytes: Vec<u8>,
    words: Vec<u64>,
}

/// Delta is the simulated delta: its directory, its header's counts, its region's words and its blocks' words, its
/// bodies by their places, and the pages and holds each process has.
struct Delta {
    budget: u64,
    slots: Vec<Slot>,
    region: [u64; blocks::WORDS],
    blocks: Vec<[u64; 3]>,
    /// hints are the free hint's words, a bit for each slot below the block past the budget's that is free.
    hints: Vec<u64>,
    /// spent counts the spent slots below the block past the budget's, and past it.
    spent: [u64; 2],
    /// unfreed counts the slots no free pass has freed, kept at each change of a slot's word, so a run's check reads
    /// it without walking the directory (bugs row 181).
    unfreed: usize,
    frees: u64,
    /// left counts the retirements since the last free pass that left nothing retired unfreed.
    left: u64,
    /// kept is the lowest stamp of a retirement a free pass kept, zero for none.
    kept: u64,
    /// waiting counts the actors asleep at the budget, or about to sleep.
    waiting: u64,
    /// overshoot is the slot plus one of the page whose compaction took a page past the budget, zero while none has.
    overshoot: u64,
    /// passes counts the free passes run, for a test of what runs one.
    passes: u64,
    /// begun counts the holds begun, for a test of how many steps a walk of the delta takes.
    begun: u64,
    /// wakes counts the frees a hold's end counted to wake the budget's waiters, for a test of what wakes them.
    wakes: u64,
    hand: u32,
    bodies: BTreeMap<u64, Body>,
    owned: Vec<(u32, Page)>,
    /// made are the records each process made and hasn't linked: its number, the aged kind's pool, and the index,
    /// the pool since a later kind's layout moves the aged kinds' numbers (bugs row 321).
    made: Vec<(u32, u32, u32)>,
    holds: Vec<(u32, u64)>,
}

impl Delta {
    /// sized returns an empty delta of a size.
    fn sized(budget: u64) -> Delta {
        Delta {
            budget,
            slots: vec![Slot::default(); page::directory_slots(budget) as usize],
            region: [0; blocks::WORDS],
            blocks: vec![[0; 3]; page::blocks(budget) as usize],
            hints: vec![u64::MAX; (page::directory_slots(budget) as usize).saturating_sub(64) / 64],
            spent: [0; 2],
            unfreed: 0,
            frees: 0,
            left: 0,
            kept: 0,
            waiting: 0,
            overshoot: 0,
            passes: 0,
            begun: 0,
            wakes: 0,
            hand: 0,
            bodies: BTreeMap::new(),
            owned: Vec::new(),
            made: Vec::new(),
            holds: Vec::new(),
        }
    }

    /// body returns the body at a place.
    fn body(&mut self, place: u64) -> &mut Body {
        self.bodies.get_mut(&place).expect("a slot names a body the region gave")
    }
}

/// DELTA is the test process's simulated delta, made at its first use.
static DELTA: Mutex<Option<Delta>> = Mutex::new(None);

/// Guarded is the simulated delta, locked.
struct Guarded(MutexGuard<'static, Option<Delta>>);

impl std::ops::Deref for Guarded {
    type Target = Delta;

    fn deref(&self) -> &Delta {
        self.0.as_ref().expect("the delta is made before it is lent")
    }
}

impl std::ops::DerefMut for Guarded {
    fn deref_mut(&mut self) -> &mut Delta {
        self.0.as_mut().expect("the delta is made before it is lent")
    }
}

/// delta returns the simulated delta, of the default size at its first use.
fn delta() -> Guarded {
    let mut delta = DELTA.lock().unwrap_or_else(PoisonError::into_inner);
    delta.get_or_insert_with(|| Delta::sized(DEFAULT));
    Guarded(delta)
}

/// at returns a field's place among a slot's other words.
fn at(field: Field) -> usize {
    match field {
        Field::Body => 0,
        Field::Sizes => 1,
        Field::Mark => 2,
        Field::Owner => 3,
        Field::Lock => 4,
        Field::Stamp => 5,
        Field::Shape => 6,
    }
}

/// internal returns the error of a seam used against its contract, which the server raises as an internal error.
fn internal(message: &str) -> Error {
    Error::fork(b"XX000", message.to_owned())
}

/// Pages is the simulation's page directory as `host`'s sequences take it.
struct Pages;

impl Directory for Pages {
    type Raise = ();

    fn slots(&self) -> u32 {
        delta().slots.len() as u32
    }

    fn budget(&self) -> u64 {
        delta().budget
    }

    fn word(&self, slot: u32) -> u64 {
        delta().slots[slot as usize].word
    }

    fn swap(&self, slot: u32, loaded: u64, word: u64) -> bool {
        let mut delta = delta();
        let held = &mut delta.slots[slot as usize].word;
        let same = *held == loaded;
        if same {
            *held = word;
            delta.unfreed =
                delta.unfreed + usize::from(shared::unfreed_word(word)) - usize::from(shared::unfreed_word(loaded));
        }
        same
    }

    fn load(&self, slot: u32, field: Field) -> u64 {
        delta().slots[slot as usize].fields[at(field)]
    }

    fn store(&self, slot: u32, field: Field, value: u64) {
        delta().slots[slot as usize].fields[at(field)] = value;
    }

    fn blocks(&self) -> u32 {
        delta().blocks.len() as u32
    }

    fn region(&self, word: Region) -> u64 {
        delta().region[word.index()]
    }

    fn set_region(&self, word: Region, value: u64) {
        delta().region[word.index()] = value;
    }

    fn block(&self, block: u32, word: Block) -> u64 {
        delta().blocks[block as usize][word as usize]
    }

    fn set_block(&self, block: u32, word: Block, value: u64) {
        delta().blocks[block as usize][word as usize] = value;
    }

    fn hint(&self, word: u32) -> u64 {
        delta().hints[word as usize]
    }

    fn hint_set(&self, slot: u32) {
        delta().hints[(slot / 64) as usize] |= 1 << (slot % 64);
    }

    fn hint_clear(&self, slot: u32) {
        delta().hints[(slot / 64) as usize] &= !(1 << (slot % 64));
    }

    fn spent(&self, over: bool) -> u64 {
        delta().spent[usize::from(over)]
    }

    fn spend(&self, over: bool) {
        delta().spent[usize::from(over)] += 1;
    }

    fn lock_region(&self) {}

    fn unlock_region(&self) {}

    fn frees(&self) -> u64 {
        delta().frees
    }

    fn freed(&self) {
        delta().frees += 1;
    }

    fn left(&self) -> u64 {
        delta().left
    }

    fn leave(&self) {
        delta().left += 1;
    }

    fn kept(&self) -> u64 {
        delta().kept
    }

    fn kept_swap(&self, loaded: u64, word: u64) -> bool {
        let mut delta = delta();
        let held = delta.kept == loaded;
        if held {
            delta.kept = word;
        }
        held
    }

    fn settle(&self, loaded: u64) {
        let mut delta = delta();
        if delta.left == loaded {
            delta.left = 0;
        }
    }

    fn waiting(&self) -> u64 {
        delta().waiting
    }

    fn count_waiter(&self, add: bool) {
        let mut delta = delta();
        delta.waiting = if add { delta.waiting + 1 } else { delta.waiting - 1 };
    }

    fn overshoot(&self) -> u64 {
        delta().overshoot
    }

    fn overshoot_swap(&self, loaded: u64, word: u64) -> bool {
        let mut delta = delta();
        if delta.overshoot != loaded {
            return false;
        }
        delta.overshoot = word;
        true
    }

    fn hand(&self) -> u32 {
        let mut delta = delta();
        let hand = delta.hand;
        delta.hand = (hand + 1) % (delta.slots.len() as u32).max(1);
        hand
    }

    fn lock(&self, (): &()) -> Result<(), Error> {
        Ok(())
    }

    fn unlock(&self) {}

    fn zero(&self, body: u64, size: Size) {
        let zeroed = Body { bytes: vec![0; size.bytes as usize], words: vec![0; size.words as usize] };
        delta().bodies.insert(body, zeroed);
    }

    fn write(&self, body: u64, at: u64, bytes: &[u8]) {
        let mut delta = delta();
        let body = delta.body(body);
        let start = at as usize - body.words.len() * size_of::<u64>();
        body.bytes[start..start + bytes.len()].copy_from_slice(bytes);
    }

    fn set(&self, body: u64, index: u64, value: u64) {
        delta().body(body).words[index as usize] = value;
    }

    fn own(&self, name: Page) {
        delta().owned.push((shared::proc(), name));
    }

    fn disown(&self, name: Page) {
        delta().owned.retain(|&(_, owned)| owned != name);
    }

    fn retiring(&self) -> u64 {
        shared::epochs_take()
    }

    fn retiring_now(&self) -> Option<u64> {
        shared::epochs_take_now()
    }

    fn retired(&self) {
        shared::epochs_advance();
        shared::epochs_unlock();
    }

    fn unretired(&self) {
        shared::epochs_unlock();
    }

    fn epoch(&self) -> u64 {
        shared::epochs_current()
    }

    fn oldest(&self) -> u64 {
        shared::epochs_oldest()
    }

    fn stop(&self, point: Point) {
        run::pause(name(point));
    }

    fn aged(&self) -> u32 {
        shared::aged()
    }

    fn aged_kind(&self, id: u64) -> Option<u32> {
        shared::aged_kind(id)
    }

    fn records(&self, kind: u32) -> u32 {
        shared::aged_records(kind)
    }

    fn record_word(&self, kind: u32, index: u32) -> u64 {
        shared::aged_word(kind, index)
    }

    fn record_swap(&self, kind: u32, index: u32, loaded: u64, word: u64) -> bool {
        shared::aged_swap(kind, index, loaded, word)
    }

    fn stamp(&self, kind: u32, index: u32) -> u64 {
        shared::aged_stamp(kind, index, None)
    }

    fn set_stamp(&self, kind: u32, index: u32, stamp: u64) {
        shared::aged_stamp(kind, index, Some(stamp));
    }

    fn record_hand(&self, kind: u32) -> u32 {
        shared::aged_hand(kind)
    }

    fn clear(&self, kind: u32, index: u32) {
        shared::aged_clear(kind, index);
    }

    fn made(&self, kind: u32, index: u32) {
        delta().made.push((shared::proc(), shared::aged_pool_of(kind), index));
    }

    fn unmade(&self, kind: u32, index: u32) {
        let pool = shared::aged_pool_of(kind);
        delta().made.retain(|&(_, made, at)| (made, at) != (pool, index));
    }
}

/// name returns the name a point of a sequence pauses at, which the server's injection point shares.
fn name(point: Point) -> &'static CStr {
    match point {
        Point::Opening => c"doltgres-delta-opening",
        Point::Stamped => c"doltgres-delta-stamped",
        Point::Claimed => c"doltgres-delta-claimed",
        Point::Passing => c"doltgres-delta-passing",
        Point::Walked => c"doltgres-delta-walked",
        Point::Stamping => c"doltgres-delta-stamping",
        Point::Oldest => c"doltgres-delta-oldest",
        Point::Waiting => c"doltgres-delta-waiting",
        Point::Hinting => c"doltgres-delta-hinting",
    }
}

/// make makes a record of an aged kind, laying the kind out at its first use, and returns its place and location,
/// or the count of frees it loaded when none was free.
pub(crate) fn make(kind: &Kind, name: &str) -> Result<Result<(u64, u64), u64>, Error> {
    run::yield_point();
    if counted(&TAKES, &()) {
        return Err(refused());
    }
    let aged = shared::aged_pool(kind)?;
    Ok(directory::make(&Pages, aged, name)?.map(|(index, location)| (shared::aged_place(aged, index), location)))
}

/// record returns the place of the record of an aged kind a location names under a hold begun at an epoch.
pub(crate) fn record(kind: &Kind, location: u64, epoch: u64) -> Result<u64, Error> {
    if counted(&RECORDS, &(kind.id, location)) {
        return Err(refused());
    }
    let aged = shared::aged_pool(kind)?;
    let index = directory::open_record(&Pages, aged, location, epoch)?;
    Ok(shared::aged_place(aged, index))
}

/// publish publishes a record the running process made.
pub(crate) fn publish(place: u64) {
    let (aged, index) = shared::aged_at(place);
    directory::publish(&Pages, aged, index);
}

/// unmake gives back a record the running process made, and does nothing for one it linked.
pub(crate) fn unmake(place: u64) {
    let (aged, index) = shared::aged_at(place);
    let pool = shared::aged_pool_of(aged);
    let made = delta().made.iter().any(|&(_, made, at)| (made, at) == (pool, index));
    if made {
        directory::unmake(&Pages, aged, index);
    }
}

/// hold begins a hold of the running process and returns its epoch.
pub(crate) fn hold() -> u64 {
    run::yield_point();
    let epoch = shared::epochs_read();
    let mut delta = delta();
    delta.holds.push((shared::proc(), epoch));
    delta.begun += 1;
    epoch
}

/// ended ends a reader of the running process, which keeps a retirement as a hold does, so its end wakes the
/// budget's waiters as a hold's does (bugs row 160).
pub(crate) fn ended(epoch: u64) {
    shared::epochs_end(epoch);
    directory::released(&Pages, epoch);
}

/// release ends a hold of the running process.
pub(crate) fn release(epoch: u64) {
    shared::epochs_end(epoch);
    let me = shared::proc();
    {
        let mut delta = delta();
        if let Some(at) = delta.holds.iter().position(|&hold| hold == (me, epoch)) {
            delta.holds.swap_remove(at);
        }
    }
    let frees = delta().frees;
    directory::released(&Pages, epoch);
    let mut delta = delta();
    if delta.frees != frees {
        delta.wakes += 1;
    }
}

/// Refusals are the calls of a door a test refuses, each a thread, what the call names, and the count of the thread's
/// calls of it to come before the one refused.
type Refusals<K> = Mutex<Vec<(ThreadId, K, u64)>>;

/// APPENDS counts down, by thread, the owned pages' appends a thread makes before one a test refuses.
static APPENDS: Refusals<()> = Mutex::new(Vec::new());

/// OPENS counts down, by thread, the opens of a page's name a thread makes before one a test refuses.
static OPENS: Refusals<Page> = Mutex::new(Vec::new());

/// RECORDS counts down, by thread, the opens of an aged record a thread makes before one a test refuses, by its
/// shape's id and its location.
static RECORDS: Refusals<(u64, u64)> = Mutex::new(Vec::new());

/// ANY counts down, by thread, the opens of any page a thread makes before one a test refuses.
static ANY: Refusals<()> = Mutex::new(Vec::new());

/// TAKES counts down, by thread, the page allocations and record makes a thread makes before one a test refuses.
static TAKES: Refusals<()> = Mutex::new(Vec::new());

/// thread returns the running thread's id, which the refusals are kept by.
fn thread() -> ThreadId {
    std::thread::current().id()
}

/// refuse_owned_append makes this thread's owned page append after a count of them fail, as an area's refusal would,
/// for a test of what an error inside a builder of owned pages leaves.
pub fn refuse_owned_append(after: u64) {
    APPENDS.lock().unwrap_or_else(PoisonError::into_inner).push((thread(), (), after));
}

/// refuse_open makes this thread's open of a page's name after a count of its opens fail, as a stale name's would,
/// for a test of what an error inside a retirement's gathering leaves.
pub fn refuse_open(name: Page, after: u64) {
    OPENS.lock().unwrap_or_else(PoisonError::into_inner).push((thread(), name, after));
}

/// refuse_record makes this thread's open of an aged record of a shape at a location after a count of its opens
/// fail, as a stale location's would, for a test of what an error before a split's first store leaves.
pub fn refuse_record(shape: u64, location: u64, after: u64) {
    RECORDS.lock().unwrap_or_else(PoisonError::into_inner).push((thread(), (shape, location), after));
}

/// refuse_any_open makes this thread's open of any page after a count of them fail, for a test that refuses each
/// open of a change in turn.
pub fn refuse_any_open(after: u64) {
    ANY.lock().unwrap_or_else(PoisonError::into_inner).push((thread(), (), after));
}

/// refuse_any_take makes this thread's page allocation or record make after a count of them fail, for a test that a
/// step takes nothing from the delta.
pub fn refuse_any_take(after: u64) {
    TAKES.lock().unwrap_or_else(PoisonError::into_inner).push((thread(), (), after));
}

/// refuse_nothing takes back every refusal this thread set that hasn't come.
pub fn refuse_nothing() {
    let me = thread();
    APPENDS.lock().unwrap_or_else(PoisonError::into_inner).retain(|(thread, ..)| *thread != me);
    OPENS.lock().unwrap_or_else(PoisonError::into_inner).retain(|(thread, ..)| *thread != me);
    RECORDS.lock().unwrap_or_else(PoisonError::into_inner).retain(|(thread, ..)| *thread != me);
    ANY.lock().unwrap_or_else(PoisonError::into_inner).retain(|(thread, ..)| *thread != me);
    TAKES.lock().unwrap_or_else(PoisonError::into_inner).retain(|(thread, ..)| *thread != me);
}

/// counted counts one door's call down for this thread, answering whether it is the one refused, once.
fn counted<K: PartialEq>(refusals: &Refusals<K>, key: &K) -> bool {
    let mut refusals = refusals.lock().unwrap_or_else(PoisonError::into_inner);
    let me = thread();
    let Some(at) = refusals.iter().position(|(thread, of, _)| *thread == me && of == key) else { return false };
    let refused = refusals[at].2 == 0;
    match refused {
        true => drop(refusals.swap_remove(at)),
        false => refusals[at].2 -= 1,
    }
    refused
}

/// refused returns the error of a door a test refused.
fn refused() -> Error {
    Error::fork(b"XX000", "a test refused the delta's door".to_owned())
}

/// open reaches the page a name names under a hold begun at an epoch.
pub(crate) fn open(name: Page, epoch: u64) -> Result<Opened, Error> {
    if counted(&OPENS, &name) | counted(&ANY, &()) {
        return Err(refused());
    }
    directory::open(&Pages, name, epoch)
}

/// room returns how many bytes an opened page takes above its filled mark.
pub(crate) fn room(opened: &Opened) -> u64 {
    directory::room(&Pages, opened)
}

/// filled returns how many bytes an opened page holds below its filled mark.
pub(crate) fn filled(opened: &Opened) -> u64 {
    directory::filled(&Pages, opened)
}

/// bytes lends a copy of a range of an opened page's bytes below its filled mark.
pub(crate) fn bytes(opened: &Opened, offset: u64, len: u64) -> Result<Bytes, Error> {
    directory::readable(&Pages, opened, offset, len)?;
    let mut delta = delta();
    let lent = &delta.body(opened.body).bytes[offset as usize..(offset + len) as usize];
    Ok(Bytes::Lent(Arc::from(lent)))
}

/// word operates on a word of a body, returning its value before, or zero for a store.
pub(crate) fn word(body: u64, index: u64, op: WordOp) -> u64 {
    let mut delta = delta();
    let word = &mut delta.body(body).words[index as usize];
    let before = *word;
    match op {
        WordOp::Load => {}
        WordOp::Store(value) => {
            *word = value;
            return 0;
        }
        WordOp::Exchange(expected, value) if before == expected => *word = value,
        WordOp::Exchange(..) => {}
        WordOp::Add(value) => *word = before.wrapping_add(value),
    }
    before
}

/// alloc takes a page of each size, all or none.
pub(crate) fn alloc(sizes: &[Size], names: &mut Vec<Page>) -> Result<Option<u64>, Error> {
    run::yield_point();
    if counted(&TAKES, &()) {
        return Err(refused());
    }
    directory::alloc(&Pages, &(), sizes, names)
}

/// alloc_replacing takes a page for the copy of a page a compaction replaces, past the budget while no other
/// replacement is.
pub(crate) fn alloc_replacing(size: Size, replaced: Page, names: &mut Vec<Page>) -> Result<Option<u64>, Error> {
    run::yield_point();
    if counted(&TAKES, &()) {
        return Err(refused());
    }
    directory::alloc_replacing(&Pages, &(), size, replaced, names)
}

/// give_back gives back a page the running process owns, and does nothing for one it linked.
pub(crate) fn give_back(name: Page) {
    let owned = delta().owned.iter().any(|&(_, owned)| owned == name);
    if owned {
        directory::give_back(&Pages, name);
    }
}

/// owned_append appends a record to a page the running process owns.
pub(crate) fn owned_append(name: Page, record: &[u8]) -> Result<u32, Error> {
    if counted(&APPENDS, &()) {
        return Err(refused());
    }
    directory::owned_append(&Pages, name, record)
}

/// owned_store stores a word of a page the running process owns.
pub(crate) fn owned_store(name: Page, index: u64, value: u64) -> Result<(), Error> {
    directory::owned_store(&Pages, name, index, value)
}

/// link records the lock that may append to an owned page and makes the page live.
pub(crate) fn link(name: Page, owner: Owner) {
    directory::link(&Pages, name, owner);
}

/// append appends a record to an opened page under the lock its slot records.
pub(crate) fn append(opened: &Opened, owner: Owner, record: &[u8]) -> Result<u32, Error> {
    run::yield_point();
    directory::append(&Pages, opened, owner, record)
}

/// seal clears an opened page's owner under the lock its slot records.
pub(crate) fn seal(opened: &Opened, owner: Owner) -> Result<(), Error> {
    run::yield_point();
    directory::seal(&Pages, opened, owner)
}

/// retire retires one batch of records and names.
pub(crate) fn retire(records: &[Leaving], names: &[Page]) {
    run::yield_point();
    directory::retire(&Pages, records, names);
}

/// pass runs the free pass.
pub(crate) fn pass() {
    delta().passes += 1;
    directory::pass(&Pages);
}

/// slots returns the number of slots of the delta's page directory.
pub(crate) fn slots() -> usize {
    delta().slots.len()
}

/// used returns the bytes the region's taken blocks hold.
pub(crate) fn used() -> u64 {
    blocks::used(&Pages)
}

/// budget returns the delta's memory in bytes.
pub(crate) fn budget() -> u64 {
    delta().budget
}

/// frees returns the count of frees, which a waiter at the budget watches.
pub(crate) fn frees() -> u64 {
    delta().frees
}

/// wait waits at the budget while the count of frees is the one given, refusing a process that has a hold, owns a
/// page, or made a record it hasn't linked, and ending with the interrupt's error when one is due; it gives up its
/// readers' epochs for the wait and runs a free pass first, so it keeps nothing it waits for (the budget wait text,
/// draft 2), and starts them again at the current epoch after.
pub(crate) fn wait(frees: u64) -> Result<(), Error> {
    let me = shared::proc();
    {
        let delta = delta();
        if delta.holds.iter().any(|&(holder, _)| holder == me) {
            return Err(internal("a backend that has a hold on the delta's pages waited at the delta's budget"));
        }
        if delta.owned.iter().any(|&(owner, _)| owner == me) {
            return Err(internal("a backend that owns a page of the delta waited at the delta's budget"));
        }
        if delta.made.iter().any(|&(maker, ..)| maker == me) {
            return Err(internal("a backend that owns an unlinked record of the delta waited at the delta's budget"));
        }
    }
    let paused = shared::epochs_paused();
    directory::pass(&Pages);
    directory::waiting(&Pages);
    let waited = slept(frees);
    directory::woken(&Pages);
    shared::epochs_resumed(paused);
    waited
}

/// slept sleeps at the budget while the count of frees is the one given, ending with the interrupt's error when one
/// is due.
fn slept(frees: u64) -> Result<(), Error> {
    loop {
        crate::files::checked()?;
        if delta().frees != frees {
            return Ok(());
        }
        if !run::wait(Part::Budget { frees }) {
            panic!("the simulation runs one actor at a time, so a wait for another actor's free would never end");
        }
    }
}

/// exit gives back the records a process made and didn't link and the pages it owns, and forgets its holds, as its
/// exit callbacks would.
pub(crate) fn exit(proc: u32) {
    let (made, left): (Vec<(u32, u32)>, Vec<Page>) = {
        let mut delta = delta();
        delta.holds.retain(|&(holder, _)| holder != proc);
        (
            delta.made.iter().filter(|&&(maker, ..)| maker == proc).map(|&(_, pool, index)| (pool, index)).collect(),
            delta.owned.iter().filter(|&&(owner, _)| owner == proc).map(|&(_, name)| name).collect(),
        )
    };
    for (pool, index) in made {
        directory::unmake(&Pages, shared::aged_of_pool(pool), index);
    }
    for name in left {
        directory::give_back(&Pages, name);
    }
}

/// reset starts the delta afresh, as a crash's restart makes all of it anew.
pub(crate) fn reset() {
    *DELTA.lock().unwrap_or_else(PoisonError::into_inner) = None;
    SEED.store(0, Ordering::Relaxed);
}

/// SEED is the running test's run's seed, which the delta's seed carries, zero outside a run.
static SEED: AtomicU64 = AtomicU64::new(0);

/// seed_delta gives the delta a run's seed, so that two runs of a seed shape the delta alike.
pub(crate) fn seed_delta(seed: u64) {
    SEED.store(seed, Ordering::Relaxed);
}

/// seed returns the delta's seed: the run's seed in its first eight bytes, and zeros after them.
pub(crate) fn seed() -> [u8; 32] {
    let mut seed = [0; 32];
    seed[..8].copy_from_slice(&SEED.load(Ordering::Relaxed).to_le_bytes());
    seed
}

/// unfreed returns how many records of an aged kind, by its shape's id, and how many pages of the directory no free
/// pass has freed, for a run's check at every yield point.
pub fn unfreed(shape: u64) -> (usize, usize) {
    (shared::aged_kind(shape).map_or(0, shared::aged_unfreed), delta().unfreed)
}

/// size_delta starts the delta afresh with a size, a test's `delta_memory`.
pub fn size_delta(budget: u64) {
    *DELTA.lock().unwrap_or_else(PoisonError::into_inner) = Some(Delta::sized(budget));
}

/// delta_passes returns the count of free passes the delta has run, for a test of what runs one (bugs row 110).
pub fn delta_passes() -> u64 {
    delta().passes
}

/// delta_holds returns the count of holds the delta has begun, for a test of how many steps a walk of it takes (bugs
/// row 123).
pub fn delta_holds() -> u64 {
    delta().begun
}

/// delta_frees returns the count of frees the delta counted, its frees' and its wakes', for a test of what a pass
/// counts (bugs row 124).
#[cfg(test)]
pub(crate) fn delta_frees() -> u64 {
    delta().frees
}

/// delta_waiting returns the count of processes asleep at the delta's budget, for a test whose actor stands in for
/// what wakes them (bugs row 124).
pub fn delta_waiting() -> u64 {
    delta().waiting
}

/// delta_wakes returns the count of frees a hold's end counted to wake the budget's waiters, for a test of what wakes
/// them (bugs row 124).
pub fn delta_wakes() -> u64 {
    delta().wakes
}

/// free_slots returns the number of free slots in the directory, for the tests.
pub fn free_slots() -> usize {
    delta().slots.iter().filter(|slot| page::state(slot.word) == page::State::Free).count()
}

/// rewind moves the directory's hand back to its first slot, for a test of a slot used again, since the slot only a
/// compaction's copy past the budget may take keeps one more slot free than a test's pages need (bugs row 110).
#[cfg(test)]
pub(crate) fn rewind() {
    delta().hand = 0;
}

/// walked counts, by a walk of the directory, the pages no free pass has freed, for a test of the count a run's check
/// reads.
#[cfg(test)]
pub(crate) fn walked() -> usize {
    delta().slots.iter().filter(|slot| shared::unfreed_word(slot.word)).count()
}

/// age stores a word in every free slot of the directory, for a test of a slot's last generation.
#[cfg(test)]
pub(crate) fn age(word: u64) {
    let mut delta = delta();
    for slot in delta.slots.iter_mut().filter(|slot| page::state(slot.word) == page::State::Free) {
        slot.word = word;
    }
    delta.unfreed = delta.slots.iter().filter(|slot| shared::unfreed_word(slot.word)).count();
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicU64, Ordering};

    use host::delta::list;
    use host::delta::page::BLOCK;
    use host::delta::{self, Full, Hold, Leaving, Made, Owned, Page, Size, With};
    use host::io::Io;
    use host::shared::{self, Aged, Atomic, Held, Limits, ListAt, Lock, Op, Pool, Removes, Shape, Single};

    use super::size_delta;
    use crate::run::{Ending, Outcome, Run};

    /// Ranged is a test's record shape: the lock that owns its pages, and two atomics that name pages.
    struct Ranged;

    impl Shape for Ranged {
        const ID: u64 = 0x7e57_de17a;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 2;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Pool for Ranged {
        const NAME: &'static str = "ranged";

        fn count(_: &Limits) -> usize {
            8
        }

        fn reclaim(_: &mut Io) -> Result<bool, host::Error> {
            Ok(false)
        }
    }

    /// Appends is the lock that owns a record's pages.
    struct Appends;

    impl Lock<Ranged> for Appends {
        const INDEX: usize = 0;
    }

    /// First is the atomic that names a record's page.
    struct First;

    impl Atomic<Ranged> for First {
        const INDEX: usize = 0;
    }

    /// Root is the atomic that names a record's list page.
    struct Root;

    impl Atomic<Ranged> for Root {
        const INDEX: usize = 1;
    }

    /// PAGE is a page of one block, 64 KiB: its bytes and eight words.
    const PAGE: Size = Size { bytes: (64 << 10) - 64, words: 8 };

    /// SMALL is a page of 8 KiB, eight of which share a block: its bytes and eight words.
    const SMALL: Size = Size { bytes: (8 << 10) - 64, words: 8 };

    /// TINY is a page of the smallest class, 1 KiB, 64 of which share a block, a slot each (bugs row 150).
    const TINY: Size = Size { bytes: (1 << 10) - 64, words: 8 };

    /// io installs the simulation's host, starts shared memory and the delta afresh, and returns a test's
    /// capability.
    fn io() -> Io {
        crate::install();
        crate::shared::reset();
        Io::testing()
    }

    /// ranged makes a record, publishes it, and opens it.
    fn ranged(io: &mut Io) -> Result<Held<Ranged>, host::Error> {
        let location = shared::make::<Ranged>(io)?.publish();
        shared::open::<Ranged>(io, location)?.ok_or_else(|| failed("a published record didn't open"))
    }

    /// failed returns a test's own error.
    fn failed(what: &str) -> host::Error {
        host::Error::fork(b"XX000", what.to_owned())
    }

    /// owned allocates pages, which the delta has room for.
    fn owned<'io>(io: &mut Io, sizes: &[Size]) -> Result<Vec<Owned<'io>>, host::Error> {
        delta::alloc(io, sizes)?.map_err(|_| failed("the delta refused pages it has room for"))
    }

    /// full allocates pages the delta has no room for, and returns its refusal.
    fn full(io: &mut Io, sizes: &[Size]) -> Full {
        match delta::alloc(io, sizes).expect("a refusal is no error") {
            Ok(_) => panic!("the delta gave pages it has no room for"),
            Err(full) => full,
        }
    }

    /// linked allocates a page and links it under a record's lock, named by the record's first atomic.
    fn linked(io: &mut Io, held: &Held<Ranged>, size: Size) -> Result<Page, host::Error> {
        let page = owned(io, &[size])?.pop().ok_or_else(|| failed("an allocation of one page gave none"))?;
        let record = held.record();
        let mut guard = record.lock::<Appends>(io)?;
        Ok(page.link(&mut guard, &record.names::<First>()))
    }

    /// appended appends a record to a page under a hold and a record's lock.
    fn appended(io: &mut Io, held: &Held<Ranged>, name: Page, record: &[u8]) -> Result<u32, host::Error> {
        let hold = delta::hold(io)?;
        let page = hold.page(name)?;
        let owner = held.record();
        let mut guard = owner.lock::<Appends>(io)?;
        page.append(&mut guard, record)
    }

    /// read reads a range of a page's bytes under a hold.
    fn read(hold: &Hold<'_>, name: Page, offset: u64, len: u64) -> Result<Vec<u8>, host::Error> {
        Ok(hold.page(name)?.bytes(offset, len)?.to_vec())
    }

    /// retire retires pages by their names.
    fn retire(io: &mut Io, names: &[Page]) -> Result<(), host::Error> {
        delta::retire(io, &[], names.iter().map(|&name| Ok(name)))
    }

    /// sqlstate returns the SQLSTATE of a result's error.
    fn sqlstate<T>(result: Result<T, host::Error>) -> Option<[u8; 5]> {
        result.err().map(|error| *error.sqlstate())
    }

    /// Bytes read back under a hold are what was appended, at the offset the append returned, across records too;
    /// a read past the mark and a word past the page's count are refused. (Cases 1 and 2.)
    #[test]
    fn bytes_read_back_are_what_was_appended() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        assert_eq!(held.record().atomic::<First>(Op::Load), name.value());
        assert_eq!(appended(io, &held, name, b"first").unwrap(), 0);
        assert_eq!(appended(io, &held, name, b"second").unwrap(), 5);
        let hold = delta::hold(io).unwrap();
        assert_eq!(read(&hold, name, 5, 6).unwrap(), b"second");
        assert_eq!(read(&hold, name, 0, 11).unwrap(), b"firstsecond");
        assert_eq!(sqlstate(read(&hold, name, 6, 6)), Some(*b"XX000"));
        let page = hold.page(name).unwrap();
        assert_eq!(page.word(7).unwrap().load(), 0);
        assert_eq!(sqlstate(page.word(8)), Some(*b"XX000"));
    }

    /// A forged name and a freed page's name are refused; a hold begun after a page's retirement can't open it, and
    /// one begun before can until it ends, after which the pass frees the page. (Cases 3, 4, and 7.)
    #[test]
    fn a_hold_reaches_only_what_was_live_when_it_began() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        appended(io, &held, name, b"kept").unwrap();
        let before = delta::hold(io).unwrap();
        assert_eq!(sqlstate(before.page(Page::named(u64::MAX))), Some(*b"XX000"));
        assert_eq!(sqlstate(before.page(Page::named(name.value() + 1))), Some(*b"XX000"));
        retire(io, &[name]).unwrap();
        let after = delta::hold(io).unwrap();
        assert_eq!(sqlstate(read(&after, name, 0, 4)), Some(*b"XX000"));
        assert_eq!(read(&before, name, 0, 4).unwrap(), b"kept");
        assert_eq!(delta::used(), PAGE.body());
        drop(after);
        retire(io, &[]).unwrap();
        assert_eq!(delta::used(), PAGE.body(), "a page was freed under a hold begun before its retirement");
        drop(before);
        retire(io, &[]).unwrap();
        assert_eq!(delta::used(), 0);
        let hold = delta::hold(io).unwrap();
        assert_eq!(sqlstate(hold.page(name)), Some(*b"XX000"));
    }

    /// A second retire of one name does nothing, and a retire of a freed page's name after its slot is used again
    /// leaves the new page live. (Case 12's first half.)
    #[test]
    fn a_retire_of_a_name_that_is_not_live_does_nothing() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        retire(io, &[name, name]).unwrap();
        retire(io, &[name]).unwrap();
        assert_eq!((delta::used(), super::free_slots()), (0, 128));
        super::rewind();
        let again = linked(io, &held, PAGE).unwrap();
        assert_eq!((again.number(), again.generation()), (name.number(), name.generation() + 1));
        retire(io, &[name]).unwrap();
        let hold = delta::hold(io).unwrap();
        assert!(hold.page(again).is_ok());
    }

    /// A second retire of a retired page's name, and a retire of a stale name of its slot, leave the page's stamp: a
    /// hold begun after the page's retirement is still refused it, and one begun before still reads it. (Case 12.)
    #[test]
    fn a_retire_of_a_name_that_is_not_live_leaves_the_stamp() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let stale = linked(io, &held, PAGE).unwrap();
        retire(io, &[stale]).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        let before = delta::hold(io).unwrap();
        retire(io, &[name]).unwrap();
        let after = delta::hold(io).unwrap();
        retire(io, &[name]).unwrap();
        retire(io, &[stale]).unwrap();
        assert_eq!(sqlstate(after.page(name)), Some(*b"XX000"));
        assert!(before.page(name).is_ok());
    }

    /// A retire of a hundred names is two advances of the epoch and one pass, and an iterator that yields an error
    /// after three names retires the three and returns that error. (Case 12's second half.)
    #[test]
    fn a_retire_takes_its_names_in_batches_and_returns_a_yielded_error() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let small = Size { bytes: 4096, words: 0 };
        let names: Vec<Page> = (0..103).map(|_| linked(io, &held, small).unwrap()).collect();
        let epoch = shared::epochs(io).unwrap().oldest();
        retire(io, &names[..100]).unwrap();
        assert_eq!(shared::epochs(io).unwrap().oldest(), epoch + 2);
        assert_eq!(delta::used(), BLOCK);
        let yielded = names[100..].iter().map(|&name| Ok(name)).chain([Err(failed("the fourth name wasn't found"))]);
        let error = delta::retire(io, &[], yielded).expect_err("the iterator's error is returned");
        assert_eq!(error.message(), "the fourth name wasn't found");
        assert_eq!(delta::used(), 0);
    }

    /// An allocation is refused, with nothing taken, when its pages together would pass the budget's blocks, and it
    /// succeeds once the frees the epochs allow have run; a size no empty delta could give is an error. (Cases 9 and
    /// 26's second half.)
    #[test]
    fn an_allocation_takes_all_its_pages_or_none() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let held = ranged(io).unwrap();
        let first = linked(io, &held, PAGE).unwrap();
        full(io, &[PAGE, PAGE]);
        assert_eq!((delta::used(), super::free_slots()), (BLOCK, 191));
        let filled = owned(io, &[SMALL; 8]).unwrap();
        full(io, &[SMALL]);
        assert_eq!((delta::used(), super::free_slots()), (2 * BLOCK, 183));
        drop(filled);
        retire(io, &[first]).unwrap();
        assert_eq!(owned(io, &[PAGE, PAGE]).unwrap().len(), 2);
        assert_eq!((delta::used(), super::free_slots()), (0, 192));
        assert_eq!(sqlstate(delta::alloc(io, &[Size { bytes: 0, words: 0 }])), Some(*b"XX000"));
        assert_eq!(sqlstate(delta::alloc(io, &[Size { bytes: (64 << 10) + 1, words: 0 }])), Some(*b"XX000"));
    }

    /// An allocation refused after its first two takes gives both slots and their blocks back, and an owned page
    /// dropped unlinked gives its slot and block back. (Case 13.)
    #[test]
    fn a_failed_allocation_and_a_dropped_page_leave_nothing_counted() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        full(io, &[PAGE, PAGE, PAGE]);
        assert_eq!((delta::used(), super::free_slots()), (0, 192));
        let pages = owned(io, &[PAGE, PAGE]).unwrap();
        assert_eq!((delta::used(), super::free_slots()), (2 * BLOCK, 190));
        drop(pages);
        assert_eq!((delta::used(), super::free_slots()), (0, 192));
    }

    /// An append under another record's guard is refused, as is one to a sealed page, and one that doesn't fit
    /// changes nothing. (Case 11.)
    #[test]
    fn an_append_needs_the_lock_that_owns_its_page() {
        let _process = crate::process();
        let io = &mut io();
        let (held, other) = (ranged(io).unwrap(), ranged(io).unwrap());
        let name = linked(io, &held, Size { bytes: 8, words: 0 }).unwrap();
        assert_eq!(sqlstate(appended(io, &other, name, b"theirs")), Some(*b"XX000"));
        assert_eq!(appended(io, &held, name, b"mine").unwrap(), 0);
        assert_eq!(sqlstate(appended(io, &held, name, b"toolong")), Some(*b"XX000"));
        assert_eq!(appended(io, &held, name, b"fits").unwrap(), 4);
        let hold = delta::hold(io).unwrap();
        let page = hold.page(name).unwrap();
        {
            let record = other.record();
            let mut theirs = record.lock::<Appends>(io).unwrap();
            assert_eq!(sqlstate(page.seal(&mut theirs)), Some(*b"XX000"));
        }
        let record = held.record();
        let mut guard = record.lock::<Appends>(io).unwrap();
        page.seal(&mut guard).unwrap();
        assert_eq!(sqlstate(page.append(&mut guard, b"")), Some(*b"XX000"));
        assert_eq!(&*page.bytes(0, 8).unwrap(), b"minefits");
    }

    /// A page's words are stored and loaded, of two exchanges from one value one wins, and an add wraps. (Case
    /// 14.)
    #[test]
    fn a_pages_words_change_after_it_is_linked() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        let hold = delta::hold(io).unwrap();
        let word = hold.page(name).unwrap().word(3).unwrap();
        word.store(7);
        assert_eq!(hold.page(name).unwrap().word(3).unwrap().load(), 7);
        assert_eq!((word.compare_exchange(7, 8), word.compare_exchange(7, 9), word.load()), (7, 8, 8));
        assert_eq!((word.fetch_add(u64::MAX), word.load()), (8, 7));
    }

    /// The count of bytes in use is the blocks that hold the owned, live, and retired pages after a mix, and two
    /// thousand 8 KiB pages fit the default delta, eight to a block. (Cases 15 and 16.)
    #[test]
    fn the_count_is_the_blocks_the_pages_hold() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let small = Size { bytes: 4096, words: 0 };
        let pages = owned(io, &[PAGE, small]).unwrap();
        let (live, retired) = (linked(io, &held, PAGE).unwrap(), linked(io, &held, small).unwrap());
        let hold = delta::hold(io).unwrap();
        retire(io, &[retired]).unwrap();
        assert_eq!(delta::used(), 3 * BLOCK);
        drop((pages, hold));
        retire(io, &[live]).unwrap();
        assert_eq!(delta::used(), 0);
        let pages = owned(io, &vec![SMALL; 2000]).unwrap();
        assert_eq!((pages.len() as u64, delta::used()), (2000, 2000 * (8 << 10)));
    }

    /// Bytes appended and a word stored while a page is owned are read back under a hold after its link, at the
    /// offset the append returned; its name is the name the link stored; and an append past its capacity and a
    /// store past its words are refused. (Cases 30's first half and 34.)
    #[test]
    fn an_owned_page_is_filled_before_its_link() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let mut page = owned(io, &[Size { bytes: 8, words: 2 }]).unwrap().pop().unwrap();
        assert_eq!((page.append(b"own").unwrap(), page.append(b"ed").unwrap()), (0, 3));
        assert_eq!(sqlstate(page.append(b"more")), Some(*b"XX000"));
        page.store(1, 99).unwrap();
        assert_eq!(sqlstate(page.store(2, 1)), Some(*b"XX000"));
        let name = page.name();
        let hold = delta::hold(io).unwrap();
        assert_eq!(sqlstate(hold.page(name)), Some(*b"XX000"), "an owned page was opened");
        let record = held.record();
        let mut guard = record.lock::<Appends>(io).unwrap();
        assert_eq!(page.link(&mut guard, &record.names::<First>()), name);
        assert_eq!(record.atomic::<First>(Op::Load), name.value());
        let opened = hold.page(name).unwrap();
        assert_eq!(&*opened.bytes(0, 5).unwrap(), b"owned");
        assert_eq!(opened.word(1).unwrap().load(), 99);
        assert_eq!(opened.append(&mut guard, b"!!!").unwrap(), 5);
    }

    /// A list page that is full gets a new root whose first word names it, stored while the root is owned, and a
    /// reader finds every old entry through the new root. (Case 30's second half.)
    #[test]
    fn a_new_root_names_the_old_one() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let list = Size { bytes: 0, words: 4 };
        let record = held.record();
        let mut pages = owned(io, &[list, list]).unwrap();
        let (mut root, mut old) = (pages.pop().unwrap(), pages.pop().unwrap());
        for entry in 0..4 {
            old.store(entry, 100 + entry).unwrap();
        }
        let hold = delta::hold(io).unwrap();
        let mut guard = record.lock::<Appends>(io).unwrap();
        let first = old.link(&mut guard, &record.names::<Root>());
        root.store(0, first.value()).unwrap();
        let grown = root.link(&mut guard, &record.names::<Root>());
        assert_eq!(record.atomic::<Root>(Op::Load), grown.value());
        let through = Page::named(hold.page(grown).unwrap().word(0).unwrap().load());
        let entries: Vec<u64> = (0..4).map(|entry| hold.page(through).unwrap().word(entry).unwrap().load()).collect();
        assert_eq!(entries, [100, 101, 102, 103]);
    }

    /// A slot freed at its last generation is spent: no allocation takes it, its last name is refused by an open,
    /// a retire of that name does nothing, and a directory whose every ordinary slot is spent refuses an allocation
    /// at once with 53200 and a hint to restart, counting no waiter (case 29; bugs row 150's H2).
    #[test]
    fn a_slot_at_its_last_generation_is_spent() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        super::age(u64::MAX >> 31 << 31);
        let held = ranged(io).unwrap();
        let names: Vec<Page> = (0..64).map(|_| linked(io, &held, TINY).unwrap()).collect();
        assert_eq!(names[0].generation(), u64::MAX >> 31);
        retire(io, &names).unwrap();
        assert_eq!((delta::used(), super::free_slots()), (0, 64));
        let hold = delta::hold(io).unwrap();
        assert_eq!(sqlstate(hold.page(names[0])), Some(*b"XX000"));
        drop(hold);
        retire(io, &names[..1]).unwrap();
        let refused = delta::alloc(io, &[TINY]);
        assert!(refused.as_ref().is_err_and(|error| error.hint().is_some_and(|hint| hint.contains("restart"))));
        assert_eq!(sqlstate(refused), Some(*b"53200"));
        assert_eq!(super::delta().waiting, 0, "no waiter is counted");
    }

    /// Bugs row 110's overshoot (orev-18 S1): a compaction's copy past the budget given back unlinked, as a compaction
    /// whose entry changed gives it back, frees the overshoot while the page it replaced stays, so a second replacement
    /// can still take one; while the first's copy stands, a second is refused.
    #[test]
    fn a_copy_past_the_budget_given_back_frees_the_overshoot() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let mut pages = Vec::new();
        while let Ok(taken) = delta::alloc(io, &[PAGE]).unwrap() {
            pages.extend(taken);
        }
        let copy = delta::alloc_replacing(io, PAGE, pages[0].name()).unwrap().ok().unwrap();
        assert!(delta::alloc_replacing(io, PAGE, pages[1].name()).unwrap().is_err());
        drop(copy);
        assert!(delta::alloc_replacing(io, PAGE, pages[1].name()).unwrap().is_ok());
    }

    /// Bugs row 124's raise (PerfPF 11's second lost wake): a pass loads the oldest epoch while a hold lives, the hold
    /// ends reading the kept stamp before the pass raises it, so its end counts no wake; the pass, which frees the one
    /// page no reader keeps and raises the stamp to the page the hold kept, reloads the oldest and counts the wake
    /// itself, one free more than the page it freed.
    #[test]
    fn a_hold_ending_between_a_passs_oldest_and_its_raise_leaves_the_pass_its_wake() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let held = ranged(io).unwrap();
        let (first, second) = (linked(io, &held, SMALL).unwrap(), linked(io, &held, SMALL).unwrap());
        let counted = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let mut run = crate::Run::new(1);
        run.actor("old", |io| {
            let hold = delta::hold(io)?;
            host::pause!(io, c"doltgres-delta-case-old")?;
            drop(hold);
            Ok(())
        });
        run.actor("first", move |io| retire(io, &[first]));
        run.actor("held", |io| {
            let hold = delta::hold(io)?;
            host::pause!(io, c"doltgres-delta-case-held")?;
            drop(hold);
            Ok(())
        });
        run.actor("second", move |io| retire(io, &[second]));
        let passed = std::sync::Arc::clone(&counted);
        run.actor("passer", move |io| {
            passed.lock().unwrap().push(super::delta_frees());
            delta::retire(io, &[], std::iter::empty())?;
            passed.lock().unwrap().push(super::delta_frees());
            Ok(())
        });
        run.until("old", c"doltgres-delta-case-old");
        run.script(&[("first", u64::MAX)]);
        run.until("held", c"doltgres-delta-case-held");
        run.script(&[("second", u64::MAX), ("old", u64::MAX)]);
        run.until("passer", c"doltgres-delta-oldest");
        run.script(&[("held", u64::MAX), ("passer", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        let counted = counted.lock().unwrap();
        assert_eq!(counted[1] - counted[0], 2, "the pass frees the first page and counts the ended hold's wake");
    }

    /// orev-18 S2, on orev-21 R1's slots: slots spent below the block past the budget's leave that block's slots free
    /// for a replacement: after a block's eight slots are spent, a block's eight allocations are given while a block
    /// is free, the next is refused, and a replacement takes one of the block past the budget's slots.
    #[test]
    fn spent_slots_leave_the_block_past_the_budgets_slots_to_a_replacement() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        super::age(u64::MAX >> 31 << 31);
        let held = ranged(io).unwrap();
        let names: Vec<Page> = (0..120).map(|_| linked(io, &held, TINY).unwrap()).collect();
        retire(io, &names).unwrap();
        let kept = delta::alloc(io, &[TINY; 8]).unwrap().ok().unwrap();
        assert!(delta::alloc(io, &[TINY]).unwrap().is_err());
        assert_eq!(delta::used(), BLOCK);
        assert!(delta::alloc_replacing(io, TINY, kept[0].name()).unwrap().is_ok());
    }

    /// The wait at the budget refuses a process that has a hold and one that owns a page, and returns at once when
    /// a free was counted after the refusal it was given. (Cases 10's refusals and 27's second half.)
    #[test]
    fn a_waiter_owns_nothing_and_misses_no_free() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let page = owned(io, &[PAGE]).unwrap();
        let owning = full(io, &[PAGE]);
        assert_eq!(sqlstate(delta::wait(io, owning)), Some(*b"XX000"));
        let refused = full(io, &[PAGE]);
        drop(page);
        let hold = delta::hold(io).unwrap();
        let holding = full(io, &[PAGE, PAGE]);
        assert_eq!(sqlstate(delta::wait(io, holding)), Some(*b"XX000"));
        drop(hold);
        delta::wait(io, refused).unwrap();
    }

    /// LINKED are the pages the writers of a run linked, which its fold retires.
    static LINKED: Mutex<Vec<Page>> = Mutex::new(Vec::new());

    /// DONE counts the writers of a run that finished.
    static DONE: AtomicU64 = AtomicU64::new(0);

    /// writes takes two pages in one allocation, waiting at the budget for as long as it is refused, links both,
    /// and lists them for the fold.
    fn writes(io: &mut Io) -> Result<(), host::Error> {
        let held = ranged(io)?;
        let pages = loop {
            match delta::alloc(io, &[PAGE, PAGE])? {
                Ok(pages) => break pages,
                Err(full) => delta::wait(io, full)?,
            }
        };
        let record = held.record();
        for page in pages {
            let mut guard = record.lock::<Appends>(io)?;
            let name = page.link(&mut guard, &record.names::<First>());
            LINKED.lock().unwrap().push(name);
        }
        DONE.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    /// folds retires what the writers linked until a number of them finished, and fails when it has turned far
    /// more often than any schedule of them needs, so that writers who never finish fail the run and don't hang it.
    fn folds(io: &mut Io, writers: u64) -> Result<(), host::Error> {
        let held = ranged(io)?;
        for _ in 0..100_000 {
            let linked = std::mem::take(&mut *LINKED.lock().unwrap());
            let finished = DONE.load(Ordering::Relaxed) == writers;
            retire(io, &linked)?;
            if finished && linked.is_empty() {
                return Ok(());
            }
            held.record().atomic::<First>(Op::Load);
        }
        Err(failed("the writers never finished"))
    }

    /// More writers than the budget has pages for, each needing two pages at once, all finish while a fold retires
    /// what they linked, under every schedule tried, and nothing stays counted. (Cases 10 and 26.)
    #[test]
    fn writers_past_the_budget_all_finish() {
        let _process = crate::process();
        for seed in 0..20 {
            let _io = io();
            size_delta(4 * BLOCK);
            LINKED.lock().unwrap().clear();
            DONE.store(0, Ordering::Relaxed);
            let mut run = Run::new(seed);
            for writer in ["a", "b", "c", "d", "e"] {
                run.actor(writer, writes);
            }
            run.actor("fold", |io| folds(io, 5));
            let report = run.finish();
            let Outcome::Ended(endings) = &report.outcome else { panic!("seed {seed} didn't end: {report:?}") };
            assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "seed {seed}: {report:?}");
            assert_eq!((delta::used(), super::free_slots()), (0, 320), "seed {seed}");
        }
    }

    /// A refusal taken while another process's free pass is held after its claim ends when that pass ends, with no
    /// fold and no retire of the waiter's. (Case 27's first half.)
    #[test]
    fn a_wait_ends_when_a_pass_in_flight_frees_its_slot() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        drop(held);
        LINKED.lock().unwrap().clear();
        LINKED.lock().unwrap().push(name);
        let mut run = Run::new(1);
        run.actor("passes", |io| retire(io, &std::mem::take(&mut *LINKED.lock().unwrap())));
        run.actor("waits", |io| {
            let full = match delta::alloc(io, &[PAGE])? {
                Ok(_) => return Err(failed("a slot a pass had claimed was allocated")),
                Err(full) => full,
            };
            delta::wait(io, full)?;
            owned(io, &[PAGE]).map(drop)
        });
        run.until("passes", c"doltgres-delta-claimed");
        run.script(&[("waits", u64::MAX), ("passes", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        assert_eq!((delta::used(), super::free_slots()), (0, 128));
    }

    /// A hold begun while a retirement is between its stamp and its advance of the epoch begins after the advance
    /// and is refused the page, and a hold begun before the retirement still reads it. (Case 5.)
    #[test]
    fn a_hold_begun_inside_a_retirement_is_refused_its_page() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        appended(io, &held, name, b"kept").unwrap();
        LINKED.lock().unwrap().clear();
        LINKED.lock().unwrap().push(name);
        let before = delta::hold(io).unwrap();
        let mut run = Run::new(1);
        run.actor("retires", |io| retire(io, &LINKED.lock().unwrap().clone()));
        run.actor("holds", |io| {
            let hold = delta::hold(io)?;
            match hold.page(LINKED.lock().unwrap()[0]) {
                Ok(_) => Err(failed("a hold begun inside a retirement opened the page")),
                Err(_) => Ok(()),
            }
        });
        run.until("retires", c"doltgres-delta-stamped");
        run.script(&[("holds", u64::MAX), ("retires", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        assert_eq!(read(&before, name, 0, 4).unwrap(), b"kept");
        assert_eq!(delta::used(), PAGE.body());
    }

    /// An actor terminated while it owns a page and has a hold leaves nothing counted, and its hold keeps no page
    /// from the next pass. (Case 13's first half.)
    #[test]
    fn a_terminated_actors_pages_and_hold_are_given_back() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        let mut run = Run::new(3);
        run.actor("terminated", |io| {
            let held = ranged(io)?;
            let (_pages, _hold) = (owned(io, &[PAGE])?, delta::hold(io)?);
            loop {
                held.record().atomic::<First>(Op::Load);
            }
        });
        run.terminate_at("terminated", 12);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!((delta::used(), super::free_slots()), (BLOCK, 191));
        retire(io, &[name]).unwrap();
        assert_eq!((delta::used(), super::free_slots()), (0, 192));
    }

    /// Solo is a test's single record shape: a lock that owns pages.
    struct Solo;

    impl Shape for Solo {
        const ID: u64 = 0x7e57_de17b;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 0;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Single for Solo {}

    /// SoloLock is the lock that owns a `Solo` record's pages.
    struct SoloLock;

    impl Lock<Solo> for SoloLock {
        const INDEX: usize = 0;
    }

    /// Duo is a second single record shape, laid out as `Solo` is.
    struct Duo;

    impl Shape for Duo {
        const ID: u64 = 0x7e57_de17c;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 0;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Single for Duo {}

    /// DuoLock is the lock that owns a `Duo` record's pages.
    struct DuoLock;

    impl Lock<Duo> for DuoLock {
        const INDEX: usize = 0;
    }

    /// A single record's lock links, appends to, and seals a page as a pooled record's does, and the locks of two
    /// single records and of a pooled record, each at index 0, are refused for one another's pages. The singles are
    /// made before the pool, so a single's place falls where the pool's first record is. (Bugs row 45.)
    #[test]
    fn a_single_records_lock_owns_its_pages_alone() {
        let _process = crate::process();
        let io = &mut io();
        let (solo, duo) = (shared::single::<Solo>().unwrap(), shared::single::<Duo>().unwrap());
        let held = ranged(io).unwrap();
        let pooled = linked(io, &held, PAGE).unwrap();
        let page = owned(io, &[PAGE]).unwrap().pop().unwrap();
        let solos = page.link(&mut solo.lock::<SoloLock>(io).unwrap(), &held.record().names::<Root>());
        let page = owned(io, &[PAGE]).unwrap().pop().unwrap();
        let duos = page.link(&mut duo.lock::<DuoLock>(io).unwrap(), &held.record().names::<Root>());
        let hold = delta::hold(io).unwrap();
        let (mine, theirs, ranged) = (hold.page(solos).unwrap(), hold.page(duos).unwrap(), hold.page(pooled).unwrap());
        assert_eq!(mine.append(&mut solo.lock::<SoloLock>(io).unwrap(), b"solo").unwrap(), 0);
        assert_eq!(theirs.append(&mut duo.lock::<DuoLock>(io).unwrap(), b"duo").unwrap(), 0);
        assert_eq!(sqlstate(mine.append(&mut duo.lock::<DuoLock>(io).unwrap(), b"duo")), Some(*b"XX000"));
        assert_eq!(sqlstate(theirs.append(&mut solo.lock::<SoloLock>(io).unwrap(), b"solo")), Some(*b"XX000"));
        assert_eq!(sqlstate(mine.append(&mut held.record().lock::<Appends>(io).unwrap(), b"pool")), Some(*b"XX000"));
        assert_eq!(sqlstate(ranged.append(&mut solo.lock::<SoloLock>(io).unwrap(), b"solo")), Some(*b"XX000"));
        assert_eq!(sqlstate(ranged.append(&mut duo.lock::<DuoLock>(io).unwrap(), b"duo")), Some(*b"XX000"));
        assert_eq!(sqlstate(mine.seal(&mut duo.lock::<DuoLock>(io).unwrap())), Some(*b"XX000"));
        assert_eq!(mine.room(), u64::from(PAGE.bytes) - 4);
        mine.seal(&mut solo.lock::<SoloLock>(io).unwrap()).unwrap();
        assert_eq!(mine.room(), 0);
        mine.seal(&mut duo.lock::<DuoLock>(io).unwrap()).unwrap();
        assert_eq!(sqlstate(mine.append(&mut solo.lock::<SoloLock>(io).unwrap(), b"solo")), Some(*b"XX000"));
    }

    /// Rows is a test's single record shape with a run of a word for each slot of the delta's page directory.
    struct Rows;

    impl Shape for Rows {
        const ID: u64 = 0x7e57_de17d;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Single for Rows {
        fn words(limits: &Limits) -> usize {
            limits.delta_pages
        }
    }

    /// RowsLock is the lock that owns a `Rows` record's pages.
    struct RowsLock;

    impl Lock<Rows> for RowsLock {
        const INDEX: usize = 0;
    }

    /// Count is the `Rows` record's one atomic, before its run.
    struct Count;

    impl Atomic<Rows> for Count {
        const INDEX: usize = 0;
    }

    /// A single record's run has a word for each slot of the directory, past its atomics: a page linked into one
    /// is reached through it under a hold, its last word is stored and loaded apart from the atomic before it, and
    /// an index past the run is refused, the largest too, which would wrap onto the record's atomic if added to
    /// their count.
    #[test]
    fn a_single_records_run_has_its_count_of_words() {
        let _process = crate::process();
        let io = &mut io();
        let rows = shared::single::<Rows>().unwrap();
        let last = super::slots() - 1;
        let mut page = owned(io, &[PAGE]).unwrap().pop().unwrap();
        page.append(b"row").unwrap();
        let word = rows.word(3).unwrap();
        let name = page.link(&mut rows.lock::<RowsLock>(io).unwrap(), &word);
        assert_eq!(word.load(), name.value());
        let hold = delta::hold(io).unwrap();
        assert_eq!(read(&hold, Page::named(word.load()), 0, 3).unwrap(), b"row");
        let end = rows.word(last).unwrap();
        end.store(9);
        assert_eq!((end.fetch_add(1), end.compare_exchange(10, 11), end.load()), (9, 10, 11));
        assert_eq!(rows.atomic::<Count>(Op::Load), 0);
        assert_eq!(sqlstate(rows.word(last + 1)), Some(*b"XX000"));
        assert_eq!(sqlstate(rows.word(usize::MAX)), Some(*b"XX000"));
    }

    /// Upper is a test's aged shape whose records name `Lower` records, a memtable's stand-in: a lock, and an atomic
    /// that names its `Lower` record.
    struct Upper;

    impl Shape for Upper {
        const ID: u64 = 0x7e57_de17e;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Aged for Upper {
        const NAME: &'static str = "upper";
        const NAMES: &'static [u64] = &[Lower::ID];

        fn count(_: &Limits) -> usize {
            4
        }
    }

    /// UpperLock is an `Upper` record's lock.
    struct UpperLock;

    impl Lock<Upper> for UpperLock {
        const INDEX: usize = 0;
    }

    /// Names is the atomic that names an `Upper` record's `Lower` record.
    struct Names;

    impl Atomic<Upper> for Names {
        const INDEX: usize = 0;
    }

    /// Lower is a test's aged shape whose records name pages, a range's stand-in: a lock that owns its pages, and an
    /// atomic that names its page.
    struct Lower;

    impl Shape for Lower {
        const ID: u64 = 0x7e57_de17f;
        const LOCKS: usize = 1;
        const FLAGS: usize = 0;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[];
        const ARRAYS: &'static [(usize, usize)] = &[];
        const LISTS: usize = 1;
    }

    /// A `Lower` record's list, under its lock, takes entries in and out.
    impl ListAt<0> for Lower {
        type Lock = LowerLock;
    }

    impl Removes<0> for Lower {}

    impl Aged for Lower {
        const NAME: &'static str = "lower";
        const NAMES: &'static [u64] = &[];

        fn count(_: &Limits) -> usize {
            4
        }
    }

    /// LowerLock is the lock that owns a `Lower` record's pages.
    struct LowerLock;

    impl Lock<Lower> for LowerLock {
        const INDEX: usize = 0;
    }

    /// Paged is the atomic that names a `Lower` record's page.
    struct Paged;

    impl Atomic<Lower> for Paged {
        const INDEX: usize = 0;
    }

    /// made makes a record of an aged shape, which the pool has room for.
    fn made<'io, S: Aged>(io: &mut Io) -> Result<Made<'io, S>, host::Error> {
        delta::make::<S>(io)?.map_err(|_| failed("an aged pool refused a record it has room for"))
    }

    /// lower makes a `Lower` record with a page that holds bytes and is named by the record's atomic, links both
    /// into a pooled record's first atomic, and returns the record's location and the page's name.
    fn lower(io: &mut Io, held: &Held<Ranged>, bytes: &[u8]) -> Result<(u64, Page), host::Error> {
        let made = made::<Lower>(io)?;
        let mut page = owned(io, &[PAGE])?.pop().ok_or_else(|| failed("an allocation of one page gave none"))?;
        page.append(bytes)?;
        let name = page.name();
        made.atomic::<Paged>(Op::Exchange(0, name.value()));
        let mut with = With::default();
        with.page::<LowerLock>(page)?;
        Ok((made.link(&held.record().names::<First>(), with), name))
    }

    /// An aged record opened under a hold begun before its retirement is reached until the hold ends, one begun
    /// after is refused, and a forged location and a freed record's are refused. (Case 20.)
    #[test]
    fn an_aged_record_is_reached_until_the_holds_before_its_retirement_end() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, name) = lower(io, &held, b"kept").unwrap();
        assert_eq!(held.record().atomic::<First>(Op::Load), location);
        let before = delta::hold(io).unwrap();
        delta::retire(io, &[Leaving::of::<Lower>(location)], [Ok(name)]).unwrap();
        let after = delta::hold(io).unwrap();
        assert_eq!(before.record::<Lower>(location).unwrap().atomic::<Paged>(Op::Load), name.value());
        assert_eq!(read(&before, name, 0, 4).unwrap(), b"kept");
        assert_eq!(sqlstate(after.record::<Lower>(location)), Some(*b"XX000"));
        assert_eq!(sqlstate(before.record::<Lower>(4 << 36)), Some(*b"XX000"));
        assert_eq!(sqlstate(before.record::<Lower>(location + 1)), Some(*b"XX000"));
        drop((before, after));
        retire(io, &[]).unwrap();
        assert_eq!(super::shared::aged_free(Lower::ID), 4);
        let hold = delta::hold(io).unwrap();
        assert_eq!(sqlstate(hold.record::<Lower>(location)), Some(*b"XX000"));
    }

    /// A make with the pool full answers `Full`; the wait refuses a process that owns an unlinked record; and once a
    /// retired record is freed the wait returns and the make succeeds. (Case 21.)
    #[test]
    fn a_full_aged_pool_answers_full_until_a_record_is_freed() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let locations: Vec<u64> = (0..4).map(|_| lower(io, &held, b"row").unwrap().0).collect();
        let Err(full) = delta::make::<Lower>(io).unwrap() else { panic!("a full pool made a record") };
        delta::retire(io, &[Leaving::of::<Lower>(locations[2])], []).unwrap();
        let unlinked = made::<Lower>(io).unwrap();
        let Err(again) = delta::make::<Lower>(io).unwrap() else { panic!("a full pool made a record") };
        assert_eq!(sqlstate(delta::wait(io, again)), Some(*b"XX000"));
        drop(unlinked);
        delta::wait(io, full).unwrap();
        assert!(made::<Lower>(io).is_ok());
    }

    /// A hold that keeps a retired record and its page frees both when it ends, the record no later than its page,
    /// and a writer that owns a page makes its record; a pass held between its walk of the records and its walk of
    /// the pages while the hold ends frees no page whose record it passed over. (Case 22.)
    #[test]
    fn a_pass_frees_a_record_no_later_than_its_page() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, name) = lower(io, &held, b"kept").unwrap();
        LINKED.lock().unwrap().clear();
        LINKED.lock().unwrap().push(name);
        let mut run = Run::new(1);
        run.actor("holds", move |io| {
            let hold = delta::hold(io)?;
            delta::retire(io, &[Leaving::of::<Lower>(location)], LINKED.lock().unwrap().clone().into_iter().map(Ok))?;
            drop(hold);
            Ok(())
        });
        run.actor("passes", |io| retire(io, &[]));
        run.until("holds", c"doltgres-delta-passing");
        run.until("passes", c"doltgres-delta-walked");
        run.script(&[("holds", u64::MAX), ("passes", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        assert_eq!((delta::used(), super::shared::aged_free(Lower::ID)), (PAGE.body(), 3));
        retire(io, &[]).unwrap();
        assert_eq!((delta::used(), super::shared::aged_free(Lower::ID)), (0, 4));
        let page = owned(io, &[PAGE]).unwrap().pop().unwrap();
        let made = made::<Lower>(io).unwrap();
        let mut with = With::default();
        with.page::<LowerLock>(page).unwrap();
        made.link(&held.record().names::<First>(), with);
    }

    /// A record retired in one call with two pages takes one advance of the epoch and one stamp on all three, so a
    /// hold begun before reaches all three and one begun after none; with a hundred pages it takes two advances, the
    /// record in the first; and an iterator that yields an error after three names retires the record and the three
    /// and returns the error. (Case 31.)
    #[test]
    fn a_record_retires_in_one_call_with_its_pages() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, first) = lower(io, &held, b"one").unwrap();
        let second = linked(io, &held, PAGE).unwrap();
        let epoch = shared::epochs(io).unwrap().oldest();
        let before = delta::hold(io).unwrap();
        delta::retire(io, &[Leaving::of::<Lower>(location)], [Ok(first), Ok(second)]).unwrap();
        let after = delta::hold(io).unwrap();
        assert!(before.record::<Lower>(location).is_ok() && before.page(first).is_ok() && before.page(second).is_ok());
        assert!(after.record::<Lower>(location).is_err() && after.page(first).is_err() && after.page(second).is_err());
        drop((before, after));
        assert_eq!(shared::epochs(io).unwrap().oldest(), epoch + 1);
        retire(io, &[]).unwrap();
        assert_eq!(delta::used(), 0);
        let small = Size { bytes: 64, words: 0 };
        let (location, _) = lower(io, &held, b"two").unwrap();
        let names: Vec<Page> = (0..100).map(|_| linked(io, &held, small).unwrap()).collect();
        let epoch = shared::epochs(io).unwrap().oldest();
        delta::retire(io, &[Leaving::of::<Lower>(location)], names.iter().map(|&name| Ok(name))).unwrap();
        assert_eq!(shared::epochs(io).unwrap().oldest(), epoch + 2);
        assert_eq!((delta::used(), super::shared::aged_free(Lower::ID)), (PAGE.body(), 4));
        let (location, _) = lower(io, &held, b"three").unwrap();
        let names: Vec<Page> = (0..3).map(|_| linked(io, &held, small).unwrap()).collect();
        let yielded = names.iter().map(|&name| Ok(name)).chain([Err(failed("the fourth name wasn't found"))]);
        let error = delta::retire(io, &[Leaving::of::<Lower>(location)], yielded).expect_err("the error is returned");
        assert_eq!(error.message(), "the fourth name wasn't found");
        assert_eq!((delta::used(), super::shared::aged_free(Lower::ID)), (2 * PAGE.body(), 4));
    }

    /// An unlinked record under an unlinked one, and the two pages filled for them, are given back by a drop and by
    /// a termination, with no retire; after a link a hold opens both records and the pages, and an append under the
    /// lower record's lock lands on its page. (Case 33.)
    #[test]
    fn an_unlinked_record_and_its_pages_are_given_back() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let built = |io: &mut Io| -> Result<(Made<'static, Upper>, With<'static, Upper>), host::Error> {
            let (upper, lower) = (made::<Upper>(io)?, made::<Lower>(io)?);
            let mut pages = owned(io, &[PAGE, PAGE])?;
            pages[0].append(b"lower")?;
            lower.atomic::<Paged>(Op::Exchange(0, pages[0].name().value()));
            upper.atomic::<Names>(Op::Exchange(0, lower.location()));
            let mut with = With::default();
            with.page::<UpperLock>(pages.pop().ok_or_else(|| failed("no second page"))?)?;
            with.record::<Lower, LowerLock>(lower, pages)?;
            Ok((upper, with))
        };
        drop(built(io).unwrap());
        assert_eq!(
            (delta::used(), super::shared::aged_free(Upper::ID), super::shared::aged_free(Lower::ID)),
            (0, 4, 4)
        );
        let mut run = Run::new(3);
        run.actor("terminated", move |io| {
            let _built = built(io)?;
            loop {
                crate::run::yield_point();
            }
        });
        run.terminate_at("terminated", 12);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!(
            (delta::used(), super::shared::aged_free(Upper::ID), super::shared::aged_free(Lower::ID)),
            (0, 4, 4)
        );
        let (upper, with) = built(io).unwrap();
        let location = upper.link(&held.record().names::<First>(), with);
        let hold = delta::hold(io).unwrap();
        let upper = hold.record::<Upper>(location).unwrap();
        let lower = hold.record::<Lower>(upper.atomic::<Names>(Op::Load)).unwrap();
        let page = hold.page(Page::named(lower.atomic::<Paged>(Op::Load))).unwrap();
        assert_eq!(&*page.bytes(0, 5).unwrap(), b"lower");
        assert_eq!(page.append(&mut lower.lock::<LowerLock>(io).unwrap(), b"more").unwrap(), 5);
    }

    /// A page an aged record's lock owns refuses an append and a seal under the lock at the same index of a record
    /// of another aged shape at the same location, since the owner names its shape too.
    #[test]
    fn an_aged_records_page_refuses_another_shapes_lock_at_its_location() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, name) = lower(io, &held, b"kept").unwrap();
        let upper = made::<Upper>(io).unwrap();
        assert_eq!(upper.location(), location);
        let upper = upper.link(&held.record().names::<Root>(), With::default());
        let hold = delta::hold(io).unwrap();
        let (upper, page) = (hold.record::<Upper>(upper).unwrap(), hold.page(name).unwrap());
        assert_eq!(sqlstate(page.append(&mut upper.lock::<UpperLock>(io).unwrap(), b"upper")), Some(*b"XX000"));
        assert_eq!(sqlstate(page.seal(&mut upper.lock::<UpperLock>(io).unwrap())), Some(*b"XX000"));
        let lower = hold.record::<Lower>(location).unwrap();
        assert_eq!(page.append(&mut lower.lock::<LowerLock>(io).unwrap(), b"lower").unwrap(), 4);
    }

    /// A record freed at its last generation is spent: a make passes it, its last location and any other are
    /// refused, a retire of it does nothing, and a pool of spent slots raises. (Case 29's aged form.)
    #[test]
    fn an_aged_record_at_its_last_generation_is_spent() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        drop(made::<Lower>(io).unwrap());
        super::shared::age_aged(Lower::ID, u64::MAX >> 31 << 31);
        let (location, name) = lower(io, &held, b"last").unwrap();
        assert_eq!(location & (u64::MAX >> 31), u64::MAX >> 31);
        delta::retire(io, &[Leaving::of::<Lower>(location)], [Ok(name)]).unwrap();
        assert_eq!(super::shared::aged_free(Lower::ID), 3);
        let hold = delta::hold(io).unwrap();
        assert_eq!(sqlstate(hold.record::<Lower>(location)), Some(*b"XX000"));
        assert_eq!(sqlstate(hold.record::<Lower>(location - 1)), Some(*b"XX000"));
        drop(hold);
        delta::retire(io, &[Leaving::of::<Lower>(location)], []).unwrap();
        assert_eq!(super::shared::aged_free(Lower::ID), 3);
        for _ in 0..3 {
            let (location, name) = lower(io, &held, b"last").unwrap();
            delta::retire(io, &[Leaving::of::<Lower>(location)], [Ok(name)]).unwrap();
        }
        assert_eq!(sqlstate(delta::make::<Lower>(io)), Some(*b"XX000"));
    }

    /// The simulation walks an aged kind before the kinds its records name, whichever a test touches first.
    #[test]
    fn the_aged_kinds_are_walked_before_what_they_name() {
        let _process = crate::process();
        let io = &mut io();
        drop(made::<Lower>(io).unwrap());
        drop(made::<Upper>(io).unwrap());
        assert!(super::shared::aged_kind(Upper::ID) < super::shared::aged_kind(Lower::ID));
    }

    /// A record made in a slot that an unlinked record left, its atomic stored, reads its atomic zeroed.
    #[test]
    fn an_aged_record_made_again_reads_its_atomics_zeroed() {
        let _process = crate::process();
        let io = &mut io();
        let dirty = made::<Lower>(io).unwrap();
        dirty.atomic::<Paged>(Op::Add(77));
        drop(dirty);
        let made: Vec<Made<'_, Lower>> = (0..4).map(|_| made::<Lower>(io).unwrap()).collect();
        assert!(made.iter().all(|made| made.atomic::<Paged>(Op::Load) == 0));
    }

    /// HELD is a location a run's actors share.
    static HELD: Mutex<u64> = Mutex::new(0);

    /// A retirement stores a page's stamp before its retired state: a free pass that loaded its oldest epoch before
    /// the retirement and walks while the retirement is held between the two stores frees nothing, and a hold begun
    /// before the retirement reads the page after both. (Bugs row 50.)
    #[test]
    fn a_page_is_stamped_before_it_is_retired() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let name = linked(io, &held, PAGE).unwrap();
        appended(io, &held, name, b"kept").unwrap();
        LINKED.lock().unwrap().clear();
        LINKED.lock().unwrap().push(name);
        let mut run = Run::new(1);
        run.actor("holds", |io| {
            let hold = delta::hold(io)?;
            crate::run::pause(c"doltgres-test-held");
            let page = hold.page(LINKED.lock().unwrap()[0])?;
            if &*page.bytes(0, 4)? != b"kept" {
                return Err(failed("a hold begun before a retirement read other bytes"));
            }
            Ok(())
        });
        run.actor("passes", |io| retire(io, &[]));
        run.actor("retires", |io| retire(io, &LINKED.lock().unwrap().clone()));
        run.until("holds", c"doltgres-test-held");
        run.until("passes", c"doltgres-delta-oldest");
        run.until("retires", c"doltgres-delta-stamping");
        run.script(&[("passes", u64::MAX), ("retires", u64::MAX), ("holds", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
    }

    /// A retirement stores a record's stamp before its retired state, as a page's: a free pass that loaded its
    /// oldest epoch before the retirement and walks while the retirement is held between the two stores frees
    /// nothing, and a hold begun before the retirement opens the record after both.
    #[test]
    fn a_record_is_stamped_before_it_is_retired() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"kept").unwrap();
        *HELD.lock().unwrap() = location;
        let mut run = Run::new(1);
        run.actor("holds", |io| {
            let hold = delta::hold(io)?;
            crate::run::pause(c"doltgres-test-held");
            hold.record::<Lower>(*HELD.lock().unwrap()).map(drop)
        });
        run.actor("passes", |io| retire(io, &[]));
        run.actor("retires", |io| delta::retire(io, &[Leaving::of::<Lower>(*HELD.lock().unwrap())], []));
        run.until("holds", c"doltgres-test-held");
        run.until("passes", c"doltgres-delta-oldest");
        run.until("retires", c"doltgres-delta-stamping");
        run.script(&[("passes", u64::MAX), ("retires", u64::MAX), ("holds", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
    }

    /// pushed pushes an entry onto a `Lower` record's list, allocating the pages the push wants before its lock, and
    /// stores the value in it.
    fn pushed(io: &mut Io, location: u64, value: u64) -> Result<(), host::Error> {
        let hold = delta::hold(io)?;
        let record = hold.record::<Lower>(location)?;
        let wants = record.lock_shared::<LowerLock>(io)?.list::<0>().wants();
        let mut spare = owned(io, &vec![list::PAGE; wants])?;
        let mut guard = record.lock::<LowerLock>(io)?;
        guard.list_mut::<0>().push(&hold, value, &mut spare)
    }

    /// entries reads a `Lower` record's list under a hold and the record's lock shared.
    fn entries(io: &mut Io, hold: &Hold<'_>, location: u64) -> Result<Vec<u64>, host::Error> {
        let record = hold.record::<Lower>(location)?;
        let guard = record.lock_shared::<LowerLock>(io)?;
        let list = guard.list::<0>();
        (0..list.len()).map(|index| list.get(hold, index)).collect()
    }

    /// A list's pages are wanted at its first page past the inline entries, at each new leaf, and at a full root,
    /// which takes a new root and a path below it, and an entry's path is its index in base 512.
    #[test]
    fn a_list_wants_a_page_where_its_tree_grows() {
        assert_eq!((list::wants(0, 0), list::wants(5, 0), list::wants(6, 0)), (0, 0, 1));
        assert_eq!((list::wants(7, 1), list::wants(517, 1), list::wants(518, 1)), (0, 0, 2));
        assert_eq!((list::wants(519, 2), list::wants(1030, 2), list::wants(1031, 2)), (0, 1, 0));
        assert_eq!(list::wants(6 + 512 * 512, 2), 3);
        assert_eq!(list::wants(6 + 512 * 512 * 512, 3), 4);
        assert_eq!(list::wants(6 + 512 * 512 + 512, 3), 1);
    }

    /// A list read back entry by entry at seven entries, in its first list page, at 519, at depth 2, and past
    /// 262,150, at depth 3, and an entry stored over reads the new value at its index. (Case 23.)
    #[test]
    fn a_list_reads_back_every_entry_at_each_depth() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for count in [7, 519, 262_151] {
            let start = {
                let hold = delta::hold(io).unwrap();
                entries(io, &hold, location).unwrap().len() as u64
            };
            for index in start..count {
                pushed(io, location, 1000 + index).unwrap();
            }
            let hold = delta::hold(io).unwrap();
            let read = entries(io, &hold, location).unwrap();
            assert!(read.iter().enumerate().all(|(index, &value)| value == 1000 + index as u64), "at {count}");
        }
        let hold = delta::hold(io).unwrap();
        let record = hold.record::<Lower>(location).unwrap();
        let mut guard = record.lock::<LowerLock>(io).unwrap();
        guard.list_mut::<0>().set(&hold, 600, 7).unwrap();
        assert_eq!(guard.list::<0>().get(&hold, 600).unwrap(), 7);
        assert_eq!(sqlstate(guard.list::<0>().get(&hold, 262_151)), Some(*b"XX000"));
    }

    /// A reader's hold keeps every old entry readable across its list's new root, which names the old one, and a
    /// store past a page's words is refused. (Case 30.)
    #[test]
    fn a_reader_finds_every_entry_after_the_root_grew() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for index in 0..518 {
            pushed(io, location, index).unwrap();
        }
        let reader = delta::hold(io).unwrap();
        let before = entries(io, &reader, location).unwrap();
        {
            let record = reader.record::<Lower>(location).unwrap();
            assert_eq!(record.lock_shared::<LowerLock>(io).unwrap().list::<0>().wants(), 2);
        }
        pushed(io, location, 518).unwrap();
        let after = entries(io, &reader, location).unwrap();
        assert_eq!((&after[..518], after[518]), (&before[..], 518));
        let mut page = owned(io, &[list::PAGE]).unwrap().pop().unwrap();
        assert_eq!(sqlstate(page.store(list::FANOUT, 1)), Some(*b"XX000"));
    }

    /// An insert moves the entries from its index up by one and a remove moves them down, and a list's length
    /// bounds its index.
    #[test]
    fn a_list_takes_entries_in_and_out() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for index in 0..10 {
            pushed(io, location, index).unwrap();
        }
        let hold = delta::hold(io).unwrap();
        let record = hold.record::<Lower>(location).unwrap();
        {
            let mut guard = record.lock::<LowerLock>(io).unwrap();
            let mut list = guard.list_mut::<0>();
            list.insert(&hold, 3, 77, &mut Vec::new()).unwrap();
            list.remove(&hold, 0).unwrap();
            assert_eq!(sqlstate(list.remove(&hold, 10)), Some(*b"XX000"));
        }
        assert_eq!(entries(io, &hold, location).unwrap(), vec![1, 2, 77, 3, 4, 5, 6, 7, 8, 9]);
    }

    /// A closed list takes no store, and its pages are the ones its record's retirement frees with the record.
    #[test]
    fn a_closed_lists_pages_retire_with_its_record() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, page) = lower(io, &held, b"list").unwrap();
        for index in 0..519 {
            pushed(io, location, index).unwrap();
        }
        let used = delta::used();
        let names: Vec<Page> = {
            let hold = delta::hold(io).unwrap();
            let record = hold.record::<Lower>(location).unwrap();
            let mut guard = record.lock::<LowerLock>(io).unwrap();
            let closed = guard.list_mut::<0>().close();
            assert_eq!(sqlstate(guard.list_mut::<0>().set(&hold, 0, 1)), Some(*b"XX000"));
            assert_eq!(sqlstate(guard.list_mut::<0>().push(&hold, 1, &mut Vec::new())), Some(*b"XX000"));
            closed.pages(&hold).collect::<Result<_, _>>().unwrap()
        };
        assert_eq!(names.len(), 3);
        delta::retire(io, &[Leaving::of::<Lower>(location)], names.into_iter().chain([page]).map(Ok)).unwrap();
        assert_eq!(delta::used(), used - 2 * BLOCK);
    }

    /// A list that shrinks and grows again reuses the pages below its reach, so every page it linked is named by its
    /// close's walk, and a retirement with its record gives the delta's use back to where it began.
    #[test]
    fn a_list_that_shrinks_and_grows_again_keeps_its_pages() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let began = delta::used();
        let (location, page) = lower(io, &held, b"list").unwrap();
        for index in 0..519 {
            pushed(io, location, index).unwrap();
        }
        let grown = delta::used();
        {
            let hold = delta::hold(io).unwrap();
            let record = hold.record::<Lower>(location).unwrap();
            let mut guard = record.lock::<LowerLock>(io).unwrap();
            guard.list_mut::<0>().remove(&hold, 518).unwrap();
            assert_eq!(guard.list::<0>().wants(), 0);
        }
        pushed(io, location, 7).unwrap();
        assert_eq!(delta::used(), grown);
        let names: Vec<Page> = {
            let hold = delta::hold(io).unwrap();
            let record = hold.record::<Lower>(location).unwrap();
            let mut guard = record.lock::<LowerLock>(io).unwrap();
            guard.list_mut::<0>().remove(&hold, 0).unwrap();
            guard.list_mut::<0>().close().pages(&hold).collect::<Result<_, _>>().unwrap()
        };
        assert_eq!(names.len(), 3);
        delta::retire(io, &[Leaving::of::<Lower>(location)], names.into_iter().chain([page]).map(Ok)).unwrap();
        assert_eq!(delta::used(), began);
    }

    /// A run's seed is the delta's, so two runs of one seed shape it alike, and outside a run the seed is zeros.
    #[test]
    fn a_runs_seed_is_the_deltas() {
        {
            let _run = Run::new(7);
            assert_eq!(&delta::seed()[..8], &7u64.to_le_bytes());
            assert_eq!(delta::seed()[8..], [0; 24]);
        }
        let _process = crate::process();
        assert_eq!(delta::seed(), [0; 32]);
    }

    /// filled makes a `Lower` record whose list is filled with entries before its link, its tree pages going live
    /// with it, and returns its location.
    fn filled(io: &mut Io, held: &Held<Ranged>, values: &[u64]) -> Result<u64, host::Error> {
        let made = made::<Lower>(io)?;
        let mut spare = owned(io, &vec![list::PAGE; list::pages(values.len() as u64)])?;
        let pages = made.fill_list::<0>(values, &mut spare)?;
        let mut with = With::default();
        for page in pages {
            with.page::<LowerLock>(page)?;
        }
        Ok(made.link(&held.record().names::<First>(), with))
    }

    /// A list filled before its record's link, of 1, 6, 7, and 519 entries, reads back entry by entry and takes a
    /// push past them, and a close's walk names its tree pages, which a retirement with its record gives back.
    #[test]
    fn a_list_filled_before_its_link_reads_back() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        assert_eq!((list::pages(6), list::pages(7), list::pages(518), list::pages(519)), (0, 1, 1, 3));
        for count in [1, 6, 7, 519] {
            let began = delta::used();
            let mut values: Vec<u64> = (0..count).map(|index| 1000 + index).collect();
            let location = filled(io, &held, &values).unwrap();
            pushed(io, location, 7).unwrap();
            values.push(7);
            let names: Vec<Page> = {
                let hold = delta::hold(io).unwrap();
                assert_eq!(entries(io, &hold, location).unwrap(), values, "at {count}");
                let record = hold.record::<Lower>(location).unwrap();
                let mut guard = record.lock::<LowerLock>(io).unwrap();
                guard.list_mut::<0>().close().pages(&hold).collect::<Result<_, _>>().unwrap()
            };
            assert_eq!(names.len(), list::pages(count + 1), "at {count}");
            delta::retire(io, &[Leaving::of::<Lower>(location)], names.into_iter().map(Ok)).unwrap();
            assert_eq!(delta::used(), began, "at {count}");
        }
        let made = made::<Lower>(io).unwrap();
        assert_eq!(sqlstate(made.fill_list::<0>(&[0; 7], &mut Vec::new())), Some(*b"XX000"));
    }

    /// A reserve grows a list's tree with the pages it wants and adds no entry, after which an insert and a push
    /// into the room it kept take no page; a reserve not given its pages is refused.
    #[test]
    fn a_reserved_lists_kept_room_takes_entries_with_no_page() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for index in 0..5 {
            pushed(io, location, index).unwrap();
        }
        let mut spare = owned(io, &[list::PAGE]).unwrap();
        let hold = delta::hold(io).unwrap();
        let record = hold.record::<Lower>(location).unwrap();
        let mut guard = record.lock::<LowerLock>(io).unwrap();
        assert_eq!(guard.list::<0>().reserving(8), 1);
        assert_eq!(sqlstate(guard.list_mut::<0>().reserve(&hold, 8, &mut Vec::new())), Some(*b"XX000"));
        guard.list_mut::<0>().reserve(&hold, 8, &mut spare).unwrap();
        assert!(spare.is_empty());
        assert_eq!((guard.list::<0>().len(), guard.list::<0>().wants()), (5, 0));
        let mut list = guard.list_mut::<0>();
        list.insert(&hold, 2, 77, &mut Vec::new()).unwrap();
        list.push(&hold, 88, &mut Vec::new()).unwrap();
        list.push(&hold, 99, &mut Vec::new()).unwrap();
        drop(guard);
        assert_eq!(entries(io, &hold, location).unwrap(), vec![0, 1, 77, 2, 3, 4, 88, 99]);
    }

    /// A reserve past a full leaf takes the new root and leaf the next push would want, and the pushes into the room
    /// it kept take no page.
    #[test]
    fn a_reserve_past_a_full_leaf_keeps_room_for_its_pushes() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for index in 0..518 {
            pushed(io, location, index).unwrap();
        }
        let mut spare = owned(io, &[list::PAGE; 2]).unwrap();
        let hold = delta::hold(io).unwrap();
        let record = hold.record::<Lower>(location).unwrap();
        let mut guard = record.lock::<LowerLock>(io).unwrap();
        assert_eq!(guard.list::<0>().reserving(520), 2);
        guard.list_mut::<0>().reserve(&hold, 520, &mut spare).unwrap();
        assert!(spare.is_empty());
        let mut list = guard.list_mut::<0>();
        list.push(&hold, 1000, &mut Vec::new()).unwrap();
        list.push(&hold, 1001, &mut Vec::new()).unwrap();
        drop(guard);
        let read = entries(io, &hold, location).unwrap();
        assert_eq!((read.len(), read[518], read[519]), (520, 1000, 1001));
    }
    /// Change is a change of a `Lower` record's list under its lock held exclusively.
    type Change<'c> = &'c dyn Fn(&Hold<'_>, &mut list::ListMut<'_, '_, Lower, LowerLock, 0>) -> Result<(), host::Error>;

    /// every_open_refused runs a change of a `Lower` record's list with each of its page opens refused in turn,
    /// checking that each refused run leaves the list as it was, until a run with none refused, and returns the list
    /// after that one.
    fn every_open_refused(io: &mut Io, location: u64, change: impl Fn(&mut Io) -> Result<(), host::Error>) -> Vec<u64> {
        let before = {
            let hold = delta::hold(io).unwrap();
            entries(io, &hold, location).unwrap()
        };
        let mut after = 0;
        loop {
            super::refuse_any_open(after);
            let changed = change(io);
            super::refuse_nothing();
            let hold = delta::hold(io).unwrap();
            let read = entries(io, &hold, location).unwrap();
            match changed {
                Ok(()) => return read,
                Err(_) => assert_eq!(read, before, "with open {after} refused"),
            }
            after += 1;
        }
    }

    /// An insert, a remove, and a replacement by two past a list's inline entries each open every entry they store
    /// before their first store: with each of their page opens refused in turn the list is as it was, and with none
    /// refused it holds the change. (R1.)
    #[test]
    fn a_list_change_with_a_refused_open_leaves_the_list_as_it_was() {
        let _process = crate::process();
        let io = &mut io();
        let held = ranged(io).unwrap();
        let (location, _) = lower(io, &held, b"list").unwrap();
        for index in 0..600 {
            pushed(io, location, index).unwrap();
        }
        let mut expected: Vec<u64> = (0..600).collect();
        let changed = |io: &mut Io, change: Change<'_>| {
            let hold = delta::hold(io)?;
            let record = hold.record::<Lower>(location)?;
            let mut guard = record.lock::<LowerLock>(io)?;
            change(&hold, &mut guard.list_mut::<0>())
        };
        let inserted =
            every_open_refused(io, location, |io| changed(io, &|hold, list| list.insert(hold, 3, 77, &mut Vec::new())));
        expected.insert(3, 77);
        assert_eq!(inserted, expected);
        let removed = every_open_refused(io, location, |io| changed(io, &|hold, list| list.remove(hold, 3)));
        expected.remove(3);
        assert_eq!(removed, expected);
        let replaced = every_open_refused(io, location, |io| {
            changed(io, &|hold, list| list.replace_with(hold, 3, [88, 99], &mut Vec::new()))
        });
        expected.splice(3..4, [88, 99]);
        assert_eq!(replaced, expected);
    }

    /// SIZES are a page of each class that shares a block: 8, 16, and 32 KiB.
    const SIZES: [Size; 3] =
        [SMALL, Size { bytes: (16 << 10) - 64, words: 8 }, Size { bytes: (32 << 10) - 64, words: 8 }];

    /// Bugs row 116: the region as TxnPF 10's dump left it, blocks of three classes each holding one live page and
    /// the rest free, gives the largest page while the blocks taken are under the budget, and at the budget a page of
    /// each class that has a free slot in its block, but none that would need a block.
    #[test]
    fn a_region_of_partly_used_blocks_gives_the_largest_page_under_the_budget() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(16 * BLOCK);
        let held = ranged(io).unwrap();
        for size in &SIZES {
            let (kept, gone) = (linked(io, &held, *size).unwrap(), linked(io, &held, *size).unwrap());
            retire(io, &[gone]).unwrap();
            assert!(delta::hold(io).unwrap().page(kept).is_ok());
        }
        assert_eq!(delta::used(), 3 * BLOCK);
        let largest = owned(io, &[PAGE; 13]).unwrap();
        assert_eq!(delta::used(), 16 * BLOCK);
        let fitted = owned(io, &SIZES).unwrap();
        full(io, &[PAGE]);
        full(io, &[SIZES[2]]);
        assert_eq!(delta::used(), 16 * BLOCK);
        drop((largest, fitted));
        assert_eq!(delta::used(), 3 * BLOCK);
    }

    /// Bugs row 116: a block's last page freed gives the block back to the free stack, so the bytes in use fall by the
    /// block and a page of another class takes it.
    #[test]
    fn a_blocks_last_page_freed_gives_the_block_to_any_class() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let (first, second) = (linked(io, &held, SMALL).unwrap(), linked(io, &held, SMALL).unwrap());
        full(io, &[PAGE]);
        retire(io, &[first]).unwrap();
        assert_eq!(delta::used(), BLOCK);
        full(io, &[PAGE]);
        retire(io, &[second]).unwrap();
        assert_eq!(delta::used(), 0);
        let page = owned(io, &[PAGE]).unwrap();
        assert_eq!(delta::used(), BLOCK);
        drop(page);
    }

    /// Bugs row 116's one budget-full state: a delta whose every block holds one small page refuses a writer of a
    /// block's page, which waits at the budget; a fold that retires those pages gives their blocks back, and the
    /// waiting writer takes its page, under every schedule tried.
    #[test]
    fn a_delta_full_of_partly_used_blocks_lets_its_waiting_writer_go_after_a_fold() {
        let _process = crate::process();
        for seed in 0..20 {
            let io = &mut io();
            size_delta(4 * BLOCK);
            let held = ranged(io).unwrap();
            let names: Vec<Page> = (0..32).map(|_| linked(io, &held, SMALL).unwrap()).collect();
            let (kept, gone): (Vec<_>, Vec<_>) = names.iter().enumerate().partition(|(at, _)| at % 8 == 0);
            retire(io, &gone.into_iter().map(|(_, &name)| name).collect::<Vec<_>>()).unwrap();
            assert_eq!(delta::used(), 4 * BLOCK);
            full(io, &[PAGE]);
            drop(held);
            *LINKED.lock().unwrap() = kept.into_iter().map(|(_, &name)| name).collect();
            let mut run = Run::new(seed);
            run.actor("writes", |io| {
                let page = loop {
                    match delta::alloc(io, &[PAGE])? {
                        Ok(mut pages) => break pages.pop(),
                        Err(full) => delta::wait(io, full)?,
                    }
                };
                drop(page);
                Ok(())
            });
            run.actor("folds", |io| retire(io, &std::mem::take(&mut *LINKED.lock().unwrap())));
            let report = run.finish();
            let Outcome::Ended(endings) = &report.outcome else { panic!("seed {seed} didn't end: {report:?}") };
            assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "seed {seed}: {report:?}");
            assert_eq!(delta::used(), 0, "seed {seed}");
        }
    }

    /// orev-21 R1: the block past the budget is the compactions' alone. With the budget full of small pages and a
    /// compaction's copy past it, an ordinary take is refused rather than fill that block; once the replaced page is
    /// freed without emptying its block and an ordinary page takes its slot again, a second compaction at the full
    /// budget is given the overshoot in that block.
    #[test]
    fn the_block_past_the_budget_holds_only_compactions_copies() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let mut pages = owned(io, &[SMALL; 16]).unwrap();
        let copy = delta::alloc_replacing(io, SMALL, pages[0].name()).unwrap().ok().unwrap();
        assert_eq!(delta::used(), 3 * BLOCK);
        full(io, &[SMALL]);
        drop(pages.remove(0));
        let refilled = owned(io, &[SMALL]).unwrap();
        full(io, &[SMALL]);
        let second = delta::alloc_replacing(io, SMALL, pages[0].name()).unwrap();
        assert!(second.is_ok());
        assert_eq!(delta::used(), 3 * BLOCK);
        drop((copy, second, refilled, pages));
        assert_eq!(delta::used(), 0);
    }

    /// orev-21 R1: once the blocks taken are back at the budget's, the block past it serves the budget, so ordinary
    /// takes fill its free slots and a compaction past the budget takes another block.
    #[test]
    fn the_block_past_the_budget_serves_the_budget_once_the_blocks_taken_are_back() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let mut pages = owned(io, &[PAGE, PAGE]).unwrap();
        let copy = delta::alloc_replacing(io, SMALL, pages[0].name()).unwrap().ok().unwrap();
        drop(pages.remove(0));
        let small = owned(io, &[SMALL; 7]).unwrap();
        assert_eq!(delta::used(), 2 * BLOCK);
        full(io, &[PAGE]);
        let second = delta::alloc_replacing(io, PAGE, pages[0].name()).unwrap();
        assert!(second.is_ok());
        assert_eq!(delta::used(), 3 * BLOCK);
        drop((copy, small, second, pages));
    }

    /// orev-21 R2: an ordinary take never fills the block past the budget, even with the directory's ordinary slots
    /// free. With the budget's blocks full of 32 KiB pages and a compaction's small copy past it, an ordinary small
    /// take is refused; once the replaced page is freed without emptying its block, a second compaction's small copy
    /// takes a slot of the block past the budget.
    #[test]
    fn an_ordinary_take_never_fills_the_block_past_the_budget() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let mut pages = owned(io, &[SIZES[2]; 4]).unwrap();
        let copy = delta::alloc_replacing(io, SMALL, pages[0].name()).unwrap().ok().unwrap();
        full(io, &[SMALL]);
        assert_eq!((delta::used(), super::free_slots()), (3 * BLOCK, 187));
        drop(pages.remove(0));
        let second = delta::alloc_replacing(io, SMALL, pages[0].name()).unwrap();
        assert!(second.is_ok());
        assert_eq!(delta::used(), 3 * BLOCK);
        drop((copy, second, pages));
    }
    /// slot_of returns the directory slot a page's name names.
    fn slot_of(name: Page) -> usize {
        name.number() as usize
    }

    /// hinted reports whether a slot's bit of the free hint is set.
    fn hinted(slot: usize) -> bool {
        super::delta().hints[slot / 64] & 1 << (slot % 64) != 0
    }

    /// Bugs row 150's free hint: a claim takes slots through their bits, clearing each, so a delta whose every
    /// ordinary slot is taken refuses at the budget, with no bit left; a release sets its slot's bit again, and the
    /// next claim, skipping the word that holds no bit, takes that slot.
    #[test]
    fn a_claim_takes_a_slot_through_the_free_hint() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(2 * BLOCK);
        let mut pages = owned(io, &[TINY; 128]).unwrap();
        assert!(pages.iter().all(|page| !hinted(slot_of(page.name()))));
        assert_eq!(super::delta().hints, [0, 0]);
        assert!(delta::alloc(io, &[TINY]).unwrap().is_err());
        let released = slot_of(pages.remove(100).name());
        assert!(hinted(released));
        let taken = owned(io, &[TINY]).unwrap();
        assert_eq!(slot_of(taken[0].name()), released);
        assert!(!hinted(released));
    }

    /// Bugs row 150's H1: a slot released at its last generation is spent, its bit of the free hint left clear, so no
    /// claim takes it.
    #[test]
    fn a_spent_slot_leaves_the_free_hint_for_good() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        super::age(u64::MAX >> 31 << 31);
        let page = owned(io, &[TINY]).unwrap().pop().unwrap();
        let slot = slot_of(page.name());
        drop(page);
        assert!(!hinted(slot));
        assert_eq!(super::delta().spent, [1, 0]);
        let pages = owned(io, &[TINY; 63]).unwrap();
        assert!(pages.iter().all(|page| slot_of(page.name()) != slot));
    }

    /// Bugs row 150's H3: a free pass held between its store of a slot's free word and its set of the slot's bit, a
    /// claim then refused for want of a bit, and the pass's free counted only after the bit, so the waiter that refusal
    /// woke finds the bit and takes the slot.
    #[test]
    fn a_waiter_woken_by_a_free_finds_its_slots_bit() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let names: Vec<Page> = (0..64).map(|_| linked(io, &held, TINY).unwrap()).collect();
        drop(held);
        LINKED.lock().unwrap().clear();
        LINKED.lock().unwrap().push(names[0]);
        let mut run = Run::new(1);
        run.actor("passes", |io| retire(io, &std::mem::take(&mut *LINKED.lock().unwrap())));
        run.actor("waits", |io| {
            let full = match delta::alloc(io, &[TINY])? {
                Ok(_) => return Err(failed("a slot whose bit isn't set yet was allocated")),
                Err(full) => full,
            };
            delta::wait(io, full)?;
            owned(io, &[TINY]).map(drop)
        });
        run.until("passes", c"doltgres-delta-hinting");
        run.script(&[("waits", u64::MAX), ("passes", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
    }

    /// Bugs row 160: a pass that loaded the epoch before another advanced it still raises the kept stamp to the
    /// retirement it kept, so the hold that keeps it wakes the budget's waiter at its end. A retired under an old hold
    /// leaves the stamp at A's, and the old hold ends with no waiter; B retired under a later hold is kept by B's pass,
    /// and by each pass of a writer refused two pages, each pass's epoch advanced before its raise. The later hold
    /// ends while the counted writer's last pass is held, and its end wakes the writer, which takes its pages, where a
    /// stamp left at A's let it sleep for good.
    #[test]
    fn a_pass_whose_epoch_moved_raises_the_kept_stamp_to_its_retirement() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let names: Vec<Page> = (0..64).map(|_| linked(io, &held, TINY).unwrap()).collect();
        drop(held);
        let (first, second) = (names[0], names[1]);
        let mut run = Run::new(1);
        run.actor("old", |io| {
            let hold = delta::hold(io)?;
            host::pause!(io, c"doltgres-delta-case-old")?;
            drop(hold);
            Ok(())
        });
        run.actor("first", move |io| retire(io, &[first]));
        run.actor("held", |io| {
            let hold = delta::hold(io)?;
            host::pause!(io, c"doltgres-delta-case-held")?;
            drop(hold);
            Ok(())
        });
        run.actor("second", move |io| retire(io, &[second]));
        for advance in ["advance 1", "advance 2", "advance 3", "advance 4"] {
            run.actor(advance, |_| {
                delta::advanced();
                Ok(())
            });
        }
        run.actor("writer", |io| {
            loop {
                match delta::alloc(io, &[TINY; 2])? {
                    Ok(pages) => {
                        drop(pages);
                        return Ok(());
                    }
                    Err(full) => delta::wait(io, full)?,
                }
            }
        });
        run.until("old", c"doltgres-delta-case-old");
        run.script(&[("first", u64::MAX), ("old", u64::MAX)]);
        run.until("held", c"doltgres-delta-case-held");
        run.until("second", c"doltgres-delta-oldest");
        run.script(&[("advance 1", u64::MAX), ("second", u64::MAX)]);
        run.until("writer", c"doltgres-delta-oldest");
        run.script(&[("advance 2", u64::MAX)]);
        run.until("writer", c"doltgres-delta-oldest");
        run.script(&[("advance 3", u64::MAX)]);
        run.until("writer", c"doltgres-delta-oldest");
        run.script(&[("advance 4", u64::MAX), ("held", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
    }

    /// Bugs row 160: a reader keeps a retirement as a hold does, so its end wakes the budget's waiter as a hold's
    /// does. A page retired while a reader lives is kept by every pass of a writer refused a page, and the reader ends
    /// while the counted writer's last pass is held; the writer wakes and takes the page, where a reader's end that
    /// woke no one let it sleep for good.
    #[test]
    fn a_readers_end_wakes_the_budgets_waiter() {
        let _process = crate::process();
        let io = &mut io();
        size_delta(BLOCK);
        let held = ranged(io).unwrap();
        let names: Vec<Page> = (0..64).map(|_| linked(io, &held, TINY).unwrap()).collect();
        drop(held);
        let first = names[0];
        let mut run = Run::new(1);
        run.actor("reader", |io| {
            let reading = shared::read(io)?;
            host::pause!(io, c"doltgres-delta-case-reader")?;
            drop(reading);
            Ok(())
        });
        run.actor("retirer", move |io| retire(io, &[first]));
        run.actor("writer", |io| {
            loop {
                match delta::alloc(io, &[TINY])? {
                    Ok(pages) => {
                        drop(pages);
                        return Ok(());
                    }
                    Err(full) => delta::wait(io, full)?,
                }
            }
        });
        run.until("reader", c"doltgres-delta-case-reader");
        run.script(&[("retirer", u64::MAX)]);
        for _ in 0..3 {
            run.until("writer", c"doltgres-delta-oldest");
        }
        run.script(&[("reader", u64::MAX)]);
        let report = run.finish();
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {report:?}") };
        assert!(endings.iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
    }

    /// Bugs row 181: the counts of the aged records and the pages no free pass has freed, which a run's check reads
    /// at every yield point, match a walk of the records' and the directory's words through makes, links, an unlinked
    /// record's drop, retirements, and the pass that frees them.
    #[test]
    fn the_unfreed_counts_match_a_walk_through_makes_retirements_and_frees() {
        let _process = crate::process();
        let io = &mut io();
        let walked = || (super::shared::aged_walked(Lower::ID), super::walked());
        let held = ranged(io).unwrap();
        let lowered: Vec<(u64, Page)> = (0..3).map(|_| lower(io, &held, b"row").unwrap()).collect();
        let unlinked = made::<Lower>(io).unwrap();
        assert_eq!(super::unfreed(Lower::ID), walked());
        assert_eq!(super::unfreed(Lower::ID).0, 4);
        drop(unlinked);
        assert_eq!(super::unfreed(Lower::ID), walked());
        for (location, name) in lowered {
            delta::retire(io, &[Leaving::of::<Lower>(location)], [Ok(name)]).unwrap();
            assert_eq!(super::unfreed(Lower::ID), walked());
        }
        retire(io, &[]).unwrap();
        assert_eq!(super::unfreed(Lower::ID), walked());
        assert_eq!(super::unfreed(Lower::ID).0, 0);
    }
}
