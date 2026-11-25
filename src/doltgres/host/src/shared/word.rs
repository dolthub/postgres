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

//! The directory word of a pooled record (design item 14's S2): a slot's generation, its holders, its state, and its
//! two marks in one `u64`, with every transition a plain function from the word a host loaded to the word it stores
//! by a compare-and-swap, so the server's host and the simulation's make the same transitions. A zeroed word is a
//! free slot at its first generation.

/// State is what a slot is to the processes that reach it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    /// Free is a slot no record is in.
    Free,
    /// Owned is a record its maker owns and never published, which no other process can find.
    Owned,
    /// Published is a record any process may open and one may claim.
    Published,
    /// Claimed is a published record a process took the ownership of, which takes no new open.
    Claimed,
    /// Condemned is a record freed while processes held it, which the last close frees.
    Condemned,
    /// Spent is a slot freed at its last generation, which is never made again.
    Spent,
}

/// STATE masks the word's state.
const STATE: u64 = 0b111;

/// RECLAIMING marks a record a reclaim is looking at.
const RECLAIMING: u64 = 1 << 3;

/// OPENED marks a record a process opened since the reclaim looking at it took it.
const OPENED: u64 = 1 << 4;

/// HOLDER is one holder in the word's count of holders.
const HOLDER: u64 = 1 << 8;

/// HOLDERS_MAX is the most holders a word counts.
pub const HOLDERS_MAX: u64 = 0xFF_FFFF;

/// GENERATION is the first bit of the word's generation, which is its upper half.
const GENERATION: u32 = 32;

/// LAST is a slot's last generation, and the mask of a location's generation.
const LAST: u64 = 0xFFFF_FFFF;

/// The state, the marks, the holders, and the generation follow one another in the word and share no bit.
const _: () = assert!(STATE < RECLAIMING && RECLAIMING < OPENED && OPENED < HOLDER);
const _: () = assert!(HOLDERS_MAX * HOLDER < 1 << GENERATION);

/// state returns a word's state.
pub fn state(word: u64) -> State {
    match word & STATE {
        0 => State::Free,
        1 => State::Owned,
        2 => State::Published,
        3 => State::Claimed,
        4 => State::Condemned,
        _ => State::Spent,
    }
}

/// holders returns a word's count of holders.
pub fn holders(word: u64) -> u64 {
    (word >> 8) & HOLDERS_MAX
}

/// generation returns a word's generation.
pub fn generation(word: u64) -> u64 {
    word >> GENERATION
}

/// location returns the location of the record in a slot of the directory with a word: the slot's number, then its
/// generation.
pub fn location(slot: u32, word: u64) -> u64 {
    u64::from(slot) << GENERATION | generation(word)
}

/// slot returns the number of the directory's slot a location names.
pub fn slot(location: u64) -> u64 {
    location >> GENERATION
}

/// names reports whether a location's generation is a word's.
fn names(word: u64, location: u64) -> bool {
    generation(word) == location & LAST
}

/// live reports whether a location's record still has its slot: the word is at the location's generation, and
/// neither free nor spent. A record freed while processes hold it has its slot until the last of them closes it.
pub fn live(word: u64, location: u64) -> bool {
    names(word, location) && !matches!(state(word), State::Free | State::Spent)
}

/// published reports whether a word is a published record's.
pub fn published(word: u64) -> bool {
    state(word) == State::Published
}

/// swept reports whether a sweep lists a slot, by the words it loaded before and after it read the record's atomic:
/// both are a published record's, at one generation, so no free and no make of the slot came between them. Their
/// holders and marks may differ, as an open or a close of the record between the loads changes them.
pub fn swept(first: u64, second: u64) -> bool {
    published(first) && published(second) && generation(first) == generation(second)
}

/// stated returns a word with another state.
fn stated(word: u64, state: State) -> u64 {
    word & !STATE | state as u64
}

/// released returns the word of a slot whose record is gone: free at the next generation, or spent at the last.
fn released(word: u64) -> u64 {
    match generation(word) {
        LAST => LAST << GENERATION | State::Spent as u64,
        generation => (generation + 1) << GENERATION,
    }
}

/// make returns the word of a free slot taken by its maker, or nothing for a slot that isn't free.
pub fn make(word: u64) -> Option<u64> {
    (state(word) == State::Free).then(|| stated(word, State::Owned))
}

/// publish returns the word of a record its owner published.
pub fn publish(word: u64) -> u64 {
    stated(word, State::Published)
}

/// claim returns the word of a published record a process claimed by its location, whatever its holders, marked as
/// opened, since its owner reaches it as a holder does, or nothing for a location that is stale or a record that
/// isn't published.
pub fn claim(word: u64, location: u64) -> Option<u64> {
    (state(word) == State::Published && names(word, location)).then(|| stated(word, State::Claimed) | OPENED)
}

/// open returns the word of a published record one more process holds, marked as opened, or nothing for a location
/// that is stale or a record that isn't published.
pub fn open(word: u64, location: u64) -> Option<u64> {
    (state(word) == State::Published && names(word, location)).then(|| (word + HOLDER) | OPENED)
}

/// close returns the word of a record one process fewer holds, which frees a condemned record no process holds any
/// more.
pub fn close(word: u64) -> u64 {
    let left = word - HOLDER;
    if state(left) == State::Condemned && holders(left) == 0 { released(left) } else { left }
}

/// free returns the word of a record its owner freed: gone when no process holds it, and condemned otherwise.
pub fn free(word: u64) -> u64 {
    if holders(word) == 0 { released(word) } else { stated(word, State::Condemned) }
}

/// give_up returns the word of a record whose owner dropped it: gone when it was never published, and published
/// again when it was claimed.
pub fn give_up(word: u64) -> u64 {
    if state(word) == State::Owned { released(word) } else { stated(word, State::Published) }
}

/// Look is what a reclaim's look at a record finds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Look {
    /// Take is the word of the record the reclaim took: one holder, reclaiming, not opened.
    Take(u64),
    /// Theirs is a record another reclaim is looking at.
    Theirs,
    /// Used is a record that is held, claimed, or condemned, or a location that is stale.
    Used,
}

/// reclaim returns what a reclaim's look at a record by its location finds: it takes a published record no process
/// holds and no reclaim is looking at.
pub fn reclaim(word: u64, location: u64) -> Look {
    if state(word) != State::Published || !names(word, location) {
        Look::Used
    } else if word & RECLAIMING != 0 {
        Look::Theirs
    } else if holders(word) != 0 {
        Look::Used
    } else {
        Look::Take(((word + HOLDER) | RECLAIMING) & !OPENED)
    }
}

/// reclaimed returns the word a reclaim's free stores over exactly the word its take stored, which is there only if no
/// process opened the record since.
pub fn reclaimed(taken: u64) -> u64 {
    released(taken)
}

/// unreclaim returns the word of a record a reclaim gave up: its holder and its mark taken off, which frees a record
/// claimed and freed meanwhile.
pub fn unreclaim(word: u64) -> u64 {
    close(word & !RECLAIMING)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// taken returns the word a reclaim's look took.
    fn taken(word: u64, location: u64) -> u64 {
        match reclaim(word, location) {
            Look::Take(taken) => taken,
            other => panic!("the look found {other:?}"),
        }
    }

    /// published returns the word of a record made and published in a slot with a word, and its location in the
    /// directory's seventh slot.
    fn published(from: u64) -> (u64, u64) {
        let word = publish(make(from).expect("a free slot is made"));
        (word, location(7, word))
    }

    /// The most holders fit their field beside every other bit below the generation, and a zeroed word is a free
    /// slot at its first generation.
    #[test]
    fn the_most_holders_fit_their_field() {
        let full = (HOLDERS_MAX * HOLDER) | STATE | RECLAIMING | OPENED;
        assert_eq!(holders(full), HOLDERS_MAX);
        assert_eq!(generation(full), 0);
        assert_eq!((state(0), holders(0), generation(0)), (State::Free, 0, 0));
        for (bits, named) in [
            (0, State::Free),
            (1, State::Owned),
            (2, State::Published),
            (3, State::Claimed),
            (4, State::Condemned),
            (5, State::Spent),
        ] {
            assert_eq!(state(bits), named);
            assert_eq!(named as u64, bits);
        }
    }

    /// A location is its slot's number and its record's generation, and only a free slot is made.
    #[test]
    fn a_location_names_a_slot_and_a_generation() {
        let (word, location) = published(5 << GENERATION);
        assert_eq!((slot(location), location & LAST), (7, 5));
        assert_eq!(state(word), State::Published);
        assert_eq!(make(word), None);
        assert_eq!(make(stated(word, State::Spent)), None);
    }

    /// A free with no holder frees the slot at the next generation, where the old location opens nothing and claims
    /// nothing.
    #[test]
    fn a_freed_record_is_gone_for_its_location() {
        let (word, location) = published(0);
        let claimed = claim(word, location).expect("a published record is claimed");
        assert_eq!(claim(claimed, location), None);
        assert_eq!(open(claimed, location), None);
        let freed = free(claimed);
        assert_eq!((state(freed), generation(freed), holders(freed)), (State::Free, 1, 0));
        let (again, _) = published(freed);
        assert_eq!(open(again, location), None);
        assert_eq!(claim(again, location), None);
        assert_eq!(reclaim(again, location), Look::Used);
    }

    /// A record freed while held is condemned, takes no open, and is gone at its last close.
    #[test]
    fn the_last_close_frees_a_condemned_record() {
        let (word, location) = published(0);
        let held = open(open(word, location).expect("opens"), location).expect("opens");
        assert_eq!(holders(held), 2);
        let condemned = free(claim(held, location).expect("claims"));
        assert_eq!((state(condemned), holders(condemned)), (State::Condemned, 2));
        assert_eq!(open(condemned, location), None);
        let one = close(condemned);
        assert_eq!((state(one), holders(one)), (State::Condemned, 1));
        let gone = close(one);
        assert_eq!((state(gone), generation(gone), holders(gone)), (State::Free, 1, 0));
    }

    /// A dropped owner frees a record never published and publishes a claimed one again, with its holders.
    #[test]
    fn a_dropped_owner_gives_its_record_up() {
        let owned = make(3 << GENERATION).expect("makes");
        assert_eq!((state(give_up(owned)), generation(give_up(owned))), (State::Free, 4));
        let (word, location) = published(0);
        let claimed = claim(open(word, location).expect("opens"), location).expect("claims");
        let back = give_up(claimed);
        assert_eq!((state(back), holders(back), generation(back)), (State::Published, 1, 0));
        assert!(open(back, location).is_some());
    }

    /// A reclaim takes only a published record no process holds, and frees it only if no process opened it since,
    /// closed again or not.
    #[test]
    fn a_reclaim_frees_only_a_record_not_opened_since() {
        let (word, location) = published(0);
        let mine = taken(word, location);
        assert_eq!(holders(mine), 1);
        assert_eq!(reclaim(mine, location), Look::Theirs);
        assert_eq!(state(reclaimed(mine)), State::Free);
        let opened = open(mine, location).expect("a record a reclaim looks at opens");
        let closed = close(opened);
        assert_ne!(closed, mine);
        assert_eq!(holders(closed), 1);
        let given = unreclaim(closed);
        assert_eq!((state(given), holders(given)), (State::Published, 0));
        assert_eq!(taken(given, location), mine);
        assert_eq!(reclaim(open(word, location).expect("opens"), location), Look::Used);
        assert_eq!(reclaim(claim(word, location).expect("claims"), location), Look::Used);
    }

    /// A record claimed while a reclaim looks at it, and published again by its dropped owner, isn't freed by the
    /// reclaim: the claim marked it opened, as its owner may have changed it.
    #[test]
    fn a_reclaim_does_not_free_a_record_claimed_since() {
        let (word, location) = published(0);
        let mine = taken(word, location);
        let back = give_up(claim(mine, location).expect("a record a reclaim looks at is claimed"));
        assert_eq!((state(back), holders(back)), (State::Published, 1));
        assert_ne!(back, mine);
        let given = unreclaim(back);
        assert_eq!((state(given), holders(given)), (State::Published, 0));
    }

    /// A reclaim given up on a record claimed and freed meanwhile frees the condemned slot.
    #[test]
    fn a_reclaim_given_up_frees_a_condemned_record() {
        let (word, location) = published(0);
        let mine = taken(word, location);
        let condemned = free(claim(mine, location).expect("claims"));
        assert_eq!(state(condemned), State::Condemned);
        assert_ne!(condemned, mine);
        let gone = unreclaim(condemned);
        assert_eq!((state(gone), generation(gone), holders(gone)), (State::Free, 1, 0));
    }

    /// A slot freed at its last generation is spent: it is never made again, and no location reaches it.
    #[test]
    fn a_slot_freed_at_its_last_generation_is_spent() {
        let (word, location) = published(LAST << GENERATION);
        assert_eq!(location & LAST, LAST);
        let spent = free(claim(word, location).expect("claims"));
        assert_eq!((state(spent), generation(spent), holders(spent)), (State::Spent, LAST, 0));
        assert_eq!(make(spent), None);
        assert_eq!(open(spent, location), None);
        assert_eq!(claim(spent, location), None);
        assert_eq!(reclaim(spent, location), Look::Used);
    }

    /// A sweep lists a record published at both of its loads, whatever opened, closed, or looked at it between
    /// them, and not one that was freed, made again, claimed, or not yet published at either.
    #[test]
    fn a_sweep_lists_a_record_published_at_both_loads() {
        let (word, location) = published(0);
        let opened = open(word, location).expect("opens");
        assert!(swept(word, word) && swept(word, opened) && swept(opened, close(opened)));
        assert!(swept(word, taken(word, location)));
        let claimed = claim(word, location).expect("claims");
        assert!(!swept(word, claimed) && !swept(claimed, word));
        assert!(swept(word, give_up(claimed)));
        let freed = free(claimed);
        assert!(!swept(word, freed) && !swept(freed, freed));
        let (again, _) = published(freed);
        assert!(!swept(word, again) && swept(again, again));
        let owned = make(0).expect("makes");
        assert!(!swept(owned, publish(owned)) && !swept(publish(owned), owned));
    }

    /// A record has its slot from its make until it is freed with no holder or its last holder closes it, and not
    /// once the slot is free, made again, or spent.
    #[test]
    fn a_record_is_live_until_its_slot_is_given_back() {
        let owned = make(0).expect("makes");
        let location = location(7, owned);
        assert!(!live(0, location));
        assert!(live(owned, location));
        let held = open(publish(owned), location).expect("opens");
        let condemned = free(claim(held, location).expect("claims"));
        assert!(live(held, location) && live(condemned, location));
        let gone = close(condemned);
        assert!(!live(gone, location));
        assert!(!live(make(gone).expect("makes"), location));
        let (last, at_last) = published(LAST << GENERATION);
        assert!(live(last, at_last));
        assert!(!live(free(claim(last, at_last).expect("claims")), at_last));
    }
}
