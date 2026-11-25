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

//! The directory word of a page of the delta (design item 14's S6): a slot's generation and its state in one `u64`,
//! a page's name, and the checks of an open, a read, an append, and the budget, each a plain function from what a
//! host loaded to what it stores or answers, so the server's host and the simulation's decide alike. A zeroed word is
//! a free slot at its first generation.

/// State is what a slot of the page directory is to the processes that reach it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    /// Free is a slot no page is in.
    Free,
    /// Owned is a page its allocator owns and hasn't linked, which no open reaches.
    Owned,
    /// Live is a page a word names, which a hold opens.
    Live,
    /// Retired is a page whose stamp is stored, which a hold begun before its retirement still opens.
    Retired,
    /// Freeing is a retired page a free pass claimed, whose body only that pass gives back.
    Freeing,
    /// Spent is a slot freed at its last generation, which is never claimed again.
    Spent,
}

/// STATE masks the word's state.
const STATE: u64 = 0b111;

/// GENERATION is the first bit of the word's generation, which is its upper 33 bits.
const GENERATION: u32 = 31;

/// LAST is a slot's last generation, and the mask of a name's generation.
const LAST: u64 = (1 << 33) - 1;

/// NUMBER is the first bit of a name's slot number, above its generation.
const NUMBER: u32 = 33;

/// SLOTS is the most slots a directory has, which the upper 31 bits of a name number.
pub const SLOTS: u64 = 1 << 31;

/// SLOT_BYTES is the memory of the delta each slot of the directory stands for: the smallest page (bugs row 150).
pub const SLOT_BYTES: u64 = 1 << 10;

/// BLOCK is a block of the delta's region, which holds pages of one size (bugs row 116): the smallest power of two
/// that holds the largest record, a blob's leaf with its version, so no record crosses a block.
pub const BLOCK: u64 = 64 << 10;

/// LARGEST is the most bytes a page's body takes, its words included: one block.
pub const LARGEST: u64 = BLOCK;

/// The most slots any `delta_memory` gives, at its largest value of `i32::MAX` kB, are numbered by a name, so an aged
/// kind's records, as many as the slots, are numbered by a location built as a name is.
const _: () = assert!(i32::MAX as u64 * 1024 / SLOT_BYTES < SLOTS);

/// The state and the generation share no bit, and a name's number and generation fill it.
const _: () = assert!(STATE < 1 << GENERATION && LAST == u64::MAX >> GENERATION && SLOTS == 1 << (64 - NUMBER));

/// state returns a word's state.
pub fn state(word: u64) -> State {
    match word & STATE {
        0 => State::Free,
        1 => State::Owned,
        2 => State::Live,
        3 => State::Retired,
        4 => State::Freeing,
        _ => State::Spent,
    }
}

/// generation returns a word's generation.
pub fn generation(word: u64) -> u64 {
    word >> GENERATION
}

/// stated returns a word with another state.
fn stated(word: u64, state: State) -> u64 {
    word & !STATE | state as u64
}

/// slots returns the number of slots the directory of a delta of a size has.
pub fn slots(delta_memory: u64) -> u64 {
    delta_memory / SLOT_BYTES
}

/// directory_slots returns the page directory's slots: one for each 1 KiB page the region's blocks can hold, the
/// block past the budget's included, so a full directory still holds the copies the compactions past the budget put
/// there (bugs rows 110 and 116).
pub fn directory_slots(delta_memory: u64) -> u64 {
    blocks(delta_memory) * (BLOCK / SLOT_BYTES)
}

/// blocks returns the blocks of the delta's region: the budget's whole blocks and one more, which only the one
/// compaction past the budget may take for its copy (bugs rows 110 and 116).
pub fn blocks(delta_memory: u64) -> u64 {
    delta_memory / BLOCK + 1
}

/// Page is a page's name: its slot's number in the directory, then the slot's generation while the page is in it. A
/// name read from a word may name no page, which the open refuses.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Page(u64);

impl Page {
    /// of returns the name of the page in a slot with a word.
    pub fn of(number: u32, word: u64) -> Page {
        Page(u64::from(number) << NUMBER | generation(word))
    }

    /// named returns the name a word holds.
    pub fn named(value: u64) -> Page {
        Page(value)
    }

    /// value returns the name as a word stores it.
    pub fn value(self) -> u64 {
        self.0
    }

    /// number returns the number of the slot the name names.
    pub fn number(self) -> u64 {
        self.0 >> NUMBER
    }

    /// generation returns the generation the name names its slot at.
    pub fn generation(self) -> u64 {
        self.0 & LAST
    }

    /// names reports whether the name's generation is a word's.
    pub fn names(self, word: u64) -> bool {
        generation(word) == self.generation()
    }
}

/// claim returns the word of a free slot its allocator took, or nothing for a slot that isn't free.
pub fn claim(word: u64) -> Option<u64> {
    (state(word) == State::Free).then(|| stated(word, State::Owned))
}

/// link returns the word of an owned page its owner linked.
pub fn link(word: u64) -> u64 {
    stated(word, State::Live)
}

/// retire returns the word of a live page a name names once it is retired, or nothing for a page that isn't live or
/// that the name doesn't name.
pub fn retire(word: u64, name: Page) -> Option<u64> {
    (state(word) == State::Live && name.names(word)).then(|| stated(word, State::Retired))
}

/// passed reports whether every published epoch has passed a retired page's stamp, given the oldest one.
pub fn passed(stamp: u64, oldest: u64) -> bool {
    stamp < oldest
}

/// reap returns the word of a retired page a free pass claimed, or nothing for a slot that isn't retired.
pub fn reap(word: u64) -> Option<u64> {
    (state(word) == State::Retired).then(|| stated(word, State::Freeing))
}

/// released returns the word of a slot whose page is gone: free at the next generation, or spent at the last.
pub fn released(word: u64) -> u64 {
    match generation(word) {
        LAST => LAST << GENERATION | State::Spent as u64,
        generation => (generation + 1) << GENERATION,
    }
}

/// Phase is what an open finds a slot to be: a page or record a hold may reach now, one retired, or neither.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Current is a live page, or a published record.
    Current,
    /// Retired is a page or record whose stamp is stored.
    Retired,
    /// Neither is any other state.
    Neither,
}

/// Seen is what one load of a slot's word shows an open: its generation and its phase.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Seen {
    /// generation is the slot's generation at the load.
    pub generation: u64,
    /// phase is the slot's phase at the load.
    pub phase: Phase,
}

/// seen returns what a load of a page's word shows an open.
pub fn seen(word: u64) -> Seen {
    let phase = match state(word) {
        State::Live => Phase::Current,
        State::Retired => Phase::Retired,
        _ => Phase::Neither,
    };
    Seen { generation: generation(word), phase }
}

/// admits reports whether an open under a hold reaches a slot, by what it loaded: the slot's word, then its stamp
/// and its other words, then its word again. The generation is the name's at both loads, so every word between
/// belongs to it; the slot is current or retired at both; and a slot found retired at the first load has a stamp no
/// older than the hold's epoch, since a hold begun after a retirement must not reach it. A slot retired between the
/// loads opens, since its retirement came after the hold began.
pub fn admits(before: Seen, stamp: u64, after: Seen, generation: u64, epoch: u64) -> bool {
    before.generation == generation
        && after.generation == generation
        && after.phase != Phase::Neither
        && match before.phase {
            Phase::Current => true,
            Phase::Retired => stamp >= epoch,
            Phase::Neither => false,
        }
}

/// Size is a page's size: the capacity of its bytes and the count of its words.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Size {
    /// bytes is the capacity of the page's bytes.
    pub bytes: u32,
    /// words is the count of the page's words.
    pub words: u32,
}

impl Size {
    /// body returns the bytes the page's body takes and the budget counts: its bytes, then its words.
    pub fn body(self) -> u64 {
        u64::from(self.bytes) + u64::from(self.words) * size_of::<u64>() as u64
    }

    /// allocates reports whether an empty area of a delta could give a page of this size: it has a body, no larger
    /// than the largest page or the delta itself.
    pub fn allocates(self, delta_memory: u64) -> bool {
        (1..=LARGEST.min(delta_memory)).contains(&self.body())
    }
}

/// lends reports whether a read of a length at an offset lies below a page's filled mark.
pub fn lends(mark: u64, offset: u64, len: u64) -> bool {
    offset.checked_add(len).is_some_and(|end| end <= mark)
}

/// appended returns a page's filled mark once a record of a length is appended above it, or nothing for a record
/// that doesn't fit the page's capacity.
pub fn appended(mark: u64, len: u64, capacity: u64) -> Option<u64> {
    mark.checked_add(len).filter(|&filled| filled <= capacity)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// linked returns the word of a page claimed and linked in a slot with a word, and its name in the directory's
    /// seventh slot.
    fn linked(from: u64) -> (u64, Page) {
        let word = link(claim(from).expect("a free slot is claimed"));
        (word, Page::of(7, word))
    }

    /// A zeroed word is a free slot at its first generation, each state has its bits, and the state and the
    /// generation don't overlap.
    #[test]
    fn a_word_holds_a_state_and_a_generation() {
        assert_eq!((state(0), generation(0)), (State::Free, 0));
        for (bits, named) in [
            (0, State::Free),
            (1, State::Owned),
            (2, State::Live),
            (3, State::Retired),
            (4, State::Freeing),
            (5, State::Spent),
        ] {
            assert_eq!(state(bits), named);
            assert_eq!(named as u64, bits);
        }
        let full = LAST << GENERATION | STATE;
        assert_eq!((generation(full), generation(STATE)), (LAST, 0));
    }

    /// A name is its slot's number in its upper 31 bits and the slot's generation in its lower 33, and the largest
    /// delta's slots are all numbered, its last slot at its last generation round-tripping (bugs row 150).
    #[test]
    fn a_name_is_a_number_and_a_generation() {
        let (word, name) = linked(5 << GENERATION);
        assert_eq!((name.number(), name.value() & LAST), (7, 5));
        assert!(name.names(word));
        assert_eq!(Page::named(name.value()), name);
        let last = Page::of((SLOTS - 1) as u32, LAST << GENERATION);
        assert_eq!((last.number(), last.value()), (SLOTS - 1, u64::MAX));
        assert!(slots(i32::MAX as u64 * 1024) <= SLOTS);
        let largest = Page::of((slots(i32::MAX as u64 * 1024) - 1) as u32, LAST << GENERATION);
        assert_eq!((largest.number(), largest.generation()), (slots(i32::MAX as u64 * 1024) - 1, LAST));
        assert_eq!(slots(32 << 20), 32 << 10);
    }

    /// Only a free slot is claimed, a retire takes a live page by its own name alone, and a pass claims only a
    /// retired page.
    #[test]
    fn each_transition_takes_only_its_own_state() {
        let owned = claim(3 << GENERATION).expect("a free slot is claimed");
        assert_eq!((state(owned), generation(owned)), (State::Owned, 3));
        assert_eq!(claim(owned), None);
        let (live, name) = linked(3 << GENERATION);
        assert_eq!((claim(live), reap(live), retire(owned, name)), (None, None, None));
        let other = Page::of(7, 4 << GENERATION);
        assert_eq!(retire(live, other), None);
        let retired = retire(live, name).expect("a live page is retired by its name");
        assert_eq!(state(retired), State::Retired);
        assert_eq!(retire(retired, name), None);
        let freeing = reap(retired).expect("a retired page is claimed by a pass");
        assert_eq!((state(freeing), reap(freeing), claim(freeing)), (State::Freeing, None, None));
    }

    /// A released slot is free at the next generation, where its old name names nothing, and a slot released at its
    /// last generation is spent: never claimed, and named by no name's retire.
    #[test]
    fn a_released_slot_moves_on_and_its_last_generation_is_spent() {
        let (live, name) = linked(3 << GENERATION);
        let free = released(reap(retire(live, name).unwrap()).unwrap());
        assert_eq!((state(free), generation(free)), (State::Free, 4));
        assert!(!name.names(free));
        assert_eq!(retire(link(claim(free).unwrap()), name), None);
        let (last, named) = linked(LAST << GENERATION);
        assert_eq!(named.value() & LAST, LAST);
        let spent = released(last);
        assert_eq!((state(spent), generation(spent)), (State::Spent, LAST));
        assert_eq!((claim(spent), retire(spent, named), reap(spent)), (None, None, None));
        assert_eq!(seen(spent).phase, Phase::Neither);
        assert_eq!(released(claim(0).unwrap()), 1 << GENERATION);
    }

    /// A pass frees a page only once every published epoch has passed its stamp.
    #[test]
    fn a_stamp_is_passed_by_a_later_oldest_epoch() {
        assert!(passed(4, 5));
        assert!(!passed(5, 5));
        assert!(!passed(6, 5));
    }

    /// An open reaches a page that is live or retired at its name's generation at both loads, a page retired
    /// before the first load only under a hold no younger than its stamp, and a page retired between the loads
    /// whatever stamp was loaded.
    #[test]
    fn an_open_reaches_a_page_a_hold_may_reach() {
        let (live, name) = linked(3 << GENERATION);
        let retired = retire(live, name).unwrap();
        let (at, was) = (seen(live), seen(retired));
        assert!(admits(at, 0, at, 3, 9));
        assert!(admits(at, 0, was, 3, 9));
        assert!(admits(was, 9, was, 3, 9));
        assert!(admits(was, 12, was, 3, 9));
        assert!(!admits(was, 8, was, 3, 9));
        assert!(!admits(at, 0, at, 4, 9));
    }

    /// An open refuses a slot whose generation moved between its two loads, with the loads scripted: the page
    /// freed and the slot claimed and linked again between them reads as live at both, at two generations.
    #[test]
    fn an_open_refuses_a_slot_remade_between_its_loads() {
        let (live, name) = linked(3 << GENERATION);
        let freeing = reap(retire(live, name).unwrap()).unwrap();
        let (again, _) = linked(released(freeing));
        assert_eq!((seen(live).phase, seen(again).phase), (Phase::Current, Phase::Current));
        assert!(!admits(seen(live), 0, seen(again), 3, 0));
        assert!(!admits(seen(live), 0, seen(freeing), 3, 0));
        assert!(!admits(seen(freeing), 0, seen(freeing), 3, 0));
        assert!(!admits(seen(claim(3 << GENERATION).unwrap()), 0, seen(live), 3, 0));
    }

    /// A page's body is its bytes and its words, a size with no body or past a block or the delta is one no empty
    /// region gives, and the region holds the budget's whole blocks and one more (bugs row 116).
    #[test]
    fn a_size_is_a_body_no_larger_than_a_block() {
        let size = Size { bytes: 8 << 10, words: 16 };
        assert_eq!(size.body(), (8 << 10) + 128);
        assert!(size.allocates(1 << 20));
        assert!(!Size { bytes: 0, words: 0 }.allocates(1 << 20));
        assert!(Size { bytes: 0, words: 512 }.allocates(1 << 20));
        assert!(Size { bytes: 64 << 10, words: 0 }.allocates(1 << 20));
        assert!(!Size { bytes: 64 << 10, words: 1 }.allocates(1 << 20));
        assert!(!Size { bytes: 8 << 10, words: 0 }.allocates(4 << 10));
        assert_eq!((blocks(1 << 20), blocks((1 << 20) + (32 << 10))), (17, 17));
    }

    /// A read lies below the filled mark, and an append fits the capacity above it.
    #[test]
    fn reads_end_at_the_mark_and_appends_at_the_capacity() {
        assert!(lends(10, 0, 10));
        assert!(lends(10, 10, 0));
        assert!(!lends(10, 4, 7));
        assert!(!lends(10, u64::MAX, 2));
        assert_eq!(appended(10, 6, 16), Some(16));
        assert_eq!(appended(10, 7, 16), None);
        assert_eq!(appended(u64::MAX, 1, u64::MAX), None);
    }
}
