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

//! The delta's region of blocks (bugs row 116): every page's body is a fixed slot of a block that holds pages of one
//! size, its class, so nothing fragments; the budget counts whole blocks taken, so a writer under the budget always
//! gets its page; and a block goes back to the free stack when its last page is freed, so no empty block is held.
//! The region's one block past the budget is the compaction's alone (orev-21 R1): only a take past the budget takes it
//! or fills its slots, and it serves the budget again once the blocks it took past are given back. A body is its place
//! in the region plus one, so zero names none.

use super::directory::Directory;
use super::page::{BLOCK, LARGEST, SLOT_BYTES, Size};

/// CLASSES is the count of page sizes a block holds: 1, 2, 4, 8, 16, 32, and 64 KiB (bugs row 150).
pub const CLASSES: u32 = 7;

/// Region is one of the region's words, which change only under the region's lock.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Region {
    /// Free is one more than the block on top of the free stack, or zero while the stack is empty.
    Free,
    /// Fresh is the count of blocks taken from the region's end, past which no block was ever taken.
    Fresh,
    /// Taken is the count of blocks holding a page.
    Taken,
    /// Over is one more than the block a take past the budget took, which no other take fills, or zero for none.
    Over,
    /// Partial is one more than the first block of a class with a free slot and a page, or zero for none.
    Partial(u32),
}

impl Region {
    /// index returns the word's place among the region's words.
    pub fn index(self) -> usize {
        match self {
            Region::Free => 0,
            Region::Fresh => 1,
            Region::Taken => 2,
            Region::Over => 3,
            Region::Partial(class) => 4 + class as usize,
        }
    }
}

/// WORDS is the count of the region's words.
pub const WORDS: usize = 4 + CLASSES as usize;

/// Block is one of a block's three words.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Block {
    /// Slots is the block's class.
    Slots,
    /// Map is the bitmap of the block's taken slots, a bit for each of its smallest class's 64 (bugs row 150).
    Map,
    /// Links is one more than the next block of its list or stack in its low half, and one more than the one before it
    /// in its class's list in its high half, or zero for none.
    Links,
}

/// A block holds whole slots of every class, the largest class is one block, and a block's slots fit its bitmap.
const _: () = assert!(LARGEST == BLOCK && SLOT_BYTES << (CLASSES - 1) == BLOCK && BLOCK / SLOT_BYTES <= 64);

/// class returns the class of a body's bytes: the power of two from 1 KiB that holds it.
pub fn class(body: u64) -> u32 {
    (body.max(SLOT_BYTES).next_power_of_two() / SLOT_BYTES).trailing_zeros()
}

/// bytes returns the bytes of a class's slot.
fn bytes(class: u32) -> u64 {
    SLOT_BYTES << class
}

/// full returns the bitmap of a block of a class whose every slot is taken.
fn full(class: u32) -> u64 {
    u64::MAX >> (64 - BLOCK / bytes(class))
}

/// budgeted returns the budget's blocks: every block of the region but the one past the budget, so a give, which runs
/// at a process's exit, reads no setting.
fn budgeted<D: Directory>(directory: &D) -> u64 {
    u64::from(directory.blocks()) - 1
}

/// used returns the bytes the region's taken blocks hold.
pub fn used<D: Directory>(directory: &D) -> u64 {
    directory.region(Region::Taken) * BLOCK
}

/// take takes a zeroed body of a page's size: a free slot of a block of its class, or else a free block while the
/// blocks taken are under the budget's. The one compaction past the budget (`overshooting`) may also take a free slot
/// of the block past the budget, or that block itself while no take holds it. It answers nothing when it refuses,
/// which happens only with the budget's every block taken.
pub fn take<D: Directory>(directory: &D, size: Size, overshooting: bool) -> Option<u64> {
    let class = class(size.body());
    directory.lock_region();
    let taken = taken(directory, class, overshooting);
    directory.unlock_region();
    let body = taken?;
    directory.zero(body, size);
    Some(body)
}

/// taken takes a slot of a class under the region's lock.
fn taken<D: Directory>(directory: &D, class: u32, overshooting: bool) -> Option<u64> {
    let partial = Region::Partial(class);
    let over = directory.region(Region::Over).checked_sub(1).map(|block| block as u32);
    let fits = |block: u32| {
        let map = directory.block(block, Block::Map);
        (directory.block(block, Block::Slots) == u64::from(class) && map != full(class)).then_some((block, map))
    };
    let (block, map) = match directory.region(partial).checked_sub(1) {
        Some(block) => (block as u32, directory.block(block as u32, Block::Map)),
        None => match over.filter(|_| overshooting).and_then(fits) {
            Some(found) => found,
            None => {
                let taken = directory.region(Region::Taken);
                match overshooting {
                    true if over.is_some() => return None,
                    false if taken - u64::from(over.is_some()) >= budgeted(directory) => return None,
                    _ => {}
                }
                let block = popped(directory)?;
                directory.set_region(Region::Taken, taken + 1);
                match overshooting {
                    true => directory.set_region(Region::Over, u64::from(block) + 1),
                    false if full(class) != 1 => pushed(directory, partial, block),
                    false => {}
                }
                (block, 0)
            }
        },
    };
    let slot = (!map).trailing_zeros();
    let map = map | 1 << slot;
    if map == full(class) && full(class) != 1 && Some(block) != over {
        unlinked(directory, partial, block);
    }
    directory.set_block(block, Block::Slots, u64::from(class));
    directory.set_block(block, Block::Map, map);
    Some(u64::from(block) * BLOCK + u64::from(slot) * bytes(class) + 1)
}

/// give gives a body back to its block, and the block to the free stack when it held its last page. Once the blocks
/// taken are back at the budget's, the block past it serves the budget, its class's list taking it while it has a free
/// slot, so a take past the budget may take another.
pub fn give<D: Directory>(directory: &D, body: u64) {
    let at = body - 1;
    let block = (at / BLOCK) as u32;
    directory.lock_region();
    let over = directory.region(Region::Over).checked_sub(1).map(|block| block as u32);
    let class = directory.block(block, Block::Slots) as u32;
    let taken = directory.block(block, Block::Map);
    let map = taken & !(1 << ((at % BLOCK) / bytes(class)));
    let listed = taken != full(class) && Some(block) != over;
    if map == 0 {
        if listed {
            unlinked(directory, Region::Partial(class), block);
        }
        if Some(block) == over {
            directory.set_region(Region::Over, 0);
        }
        directory.set_block(block, Block::Slots, 0);
        directory.set_block(block, Block::Map, 0);
        directory.set_block(block, Block::Links, directory.region(Region::Free));
        directory.set_region(Region::Free, u64::from(block) + 1);
        directory.set_region(Region::Taken, directory.region(Region::Taken) - 1);
    } else {
        directory.set_block(block, Block::Map, map);
        if !listed && Some(block) != over {
            pushed(directory, Region::Partial(class), block);
        }
    }
    if let Some(over) = directory.region(Region::Over).checked_sub(1).map(|block| block as u32)
        && directory.region(Region::Taken) <= budgeted(directory)
    {
        directory.set_region(Region::Over, 0);
        let class = directory.block(over, Block::Slots) as u32;
        if directory.block(over, Block::Map) != full(class) {
            pushed(directory, Region::Partial(class), over);
        }
    }
    directory.unlock_region();
}

/// popped takes a block off the free stack, or else the region's next fresh block.
fn popped<D: Directory>(directory: &D) -> Option<u32> {
    match directory.region(Region::Free).checked_sub(1) {
        Some(block) => {
            let block = block as u32;
            directory.set_region(Region::Free, directory.block(block, Block::Links) & u64::from(u32::MAX));
            directory.set_block(block, Block::Links, 0);
            Some(block)
        }
        None => {
            let fresh = directory.region(Region::Fresh);
            (fresh < u64::from(directory.blocks())).then(|| {
                directory.set_region(Region::Fresh, fresh + 1);
                fresh as u32
            })
        }
    }
}

/// pushed puts a block first in its class's list.
fn pushed<D: Directory>(directory: &D, list: Region, block: u32) {
    let next = directory.region(list);
    if let Some(after) = next.checked_sub(1) {
        let links = directory.block(after as u32, Block::Links);
        directory.set_block(after as u32, Block::Links, links & u64::from(u32::MAX) | (u64::from(block) + 1) << 32);
    }
    directory.set_block(block, Block::Links, next);
    directory.set_region(list, u64::from(block) + 1);
}

/// unlinked takes a block out of its class's list.
fn unlinked<D: Directory>(directory: &D, list: Region, block: u32) {
    let links = directory.block(block, Block::Links);
    let (next, before) = (links & u64::from(u32::MAX), links >> 32);
    match before.checked_sub(1) {
        Some(before) => {
            let kept = directory.block(before as u32, Block::Links) >> 32 << 32;
            directory.set_block(before as u32, Block::Links, kept | next);
        }
        None => directory.set_region(list, next),
    }
    if let Some(after) = next.checked_sub(1) {
        let kept = directory.block(after as u32, Block::Links) & u64::from(u32::MAX);
        directory.set_block(after as u32, Block::Links, kept | before << 32);
    }
    directory.set_block(block, Block::Links, 0);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A body's class is the power of two from 1 KiB that holds it, a block holds 64 of the smallest and one of the
    /// largest, and a full block's bitmap is its every slot (bugs row 150).
    #[test]
    fn a_body_takes_the_smallest_class_that_holds_it() {
        let classes: Vec<u32> =
            [1, 1 << 10, (1 << 10) + 1, 8 << 10, (8 << 10) + 1, 64 << 10].into_iter().map(class).collect();
        assert_eq!(classes, [0, 0, 1, 3, 4, 6]);
        assert_eq!((full(0), full(3), full(6)), (u64::MAX, 0xff, 1));
    }
}
