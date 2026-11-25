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

//! The simulation's libzstd and its allocator: the wrappers over libzstd the simulation compresses with, and the
//! global allocator, which counts the heap in use and fails every request while a test asks it to.

use std::alloc::{GlobalAlloc, Layout, System};
use std::ptr;
#[cfg(feature = "outside_postgres")]
use std::sync::atomic::AtomicBool;
use std::sync::atomic::{AtomicUsize, Ordering};

/// bindgen's declarations of libzstd's functions and types.
#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code,
    unsafe_op_in_unsafe_fn,
    clippy::missing_safety_doc
)]
pub mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub mod zstd;

// The libzstd Postgres links, which a test binary, linked without Postgres, links itself.
#[cfg(any(test, feature = "outside_postgres"))]
#[link(name = "zstd")]
unsafe extern "C" {}

/// FAILING is whether the allocator fails every request, which the fork's tests set in a process of one thread.
#[cfg(feature = "outside_postgres")]
static FAILING: AtomicBool = AtomicBool::new(false);

/// fail_allocations makes every allocation fail as one the system refuses does, or stops that, for the fork's tests
/// of what a failed allocation leaves behind, in a process of one thread.
#[cfg(feature = "outside_postgres")]
pub fn fail_allocations(fail: bool) {
    FAILING.store(fail, Ordering::SeqCst);
}

/// IN_USE is the bytes the allocator gave and has not been given back, which a test binary built without Postgres
/// counts.
#[cfg(any(test, feature = "outside_postgres"))]
static IN_USE: AtomicUsize = AtomicUsize::new(0);

/// heap_in_use returns the bytes the allocator gave and has not been given back, for the fork's tests of what a piece
/// of work leaves on the heap.
#[cfg(any(test, feature = "outside_postgres"))]
pub fn heap_in_use() -> usize {
    IN_USE.load(Ordering::Relaxed)
}

/// count counts the bytes of a block the system gave, if it gave one, less the bytes given back for it.
#[cfg(any(test, feature = "outside_postgres"))]
fn count(block: *mut u8, given: usize, back: usize) {
    if !block.is_null() {
        IN_USE.fetch_add(given.wrapping_sub(back), Ordering::Relaxed);
    }
}

/// refused reports whether the fork's tests fail this request.
fn refused() -> bool {
    #[cfg(feature = "outside_postgres")]
    if FAILING.load(Ordering::SeqCst) {
        return true;
    }
    false
}

/// Counting is the global allocator: the system's, which counts the bytes in use and fails what a test asks it to.
struct Counting;

// SAFETY: every method passes its arguments to `System` unchanged and returns what it returns, or returns null, as
// the system does for a request it refuses, while the fork's tests fail every request. Tested by
// tests::the_allocator_passes_requests_through; sim/tests/pin_table_growth.rs: a failed allocation growing the pin table
// is refused.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if refused() {
            return ptr::null_mut();
        }
        // SAFETY: the caller meets `alloc`'s contract, which `System.alloc` shares. Tested by
        // tests::the_allocator_passes_requests_through.
        let block = unsafe { System.alloc(layout) };
        #[cfg(any(test, feature = "outside_postgres"))]
        count(block, layout.size(), 0);
        block
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if refused() {
            return ptr::null_mut();
        }
        // SAFETY: the caller meets `alloc_zeroed`'s contract, which `System.alloc_zeroed` shares. Tested by
        // tests::the_allocator_passes_requests_through.
        let block = unsafe { System.alloc_zeroed(layout) };
        #[cfg(any(test, feature = "outside_postgres"))]
        count(block, layout.size(), 0);
        block
    }

    unsafe fn realloc(&self, block: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if refused() {
            return ptr::null_mut();
        }
        // SAFETY: the caller meets `realloc`'s contract, which `System.realloc` shares. Tested by
        // tests::the_allocator_passes_requests_through.
        let moved = unsafe { System.realloc(block, layout, size) };
        #[cfg(any(test, feature = "outside_postgres"))]
        count(moved, size, layout.size());
        moved
    }

    unsafe fn dealloc(&self, block: *mut u8, layout: Layout) {
        #[cfg(any(test, feature = "outside_postgres"))]
        count(block, 0, layout.size());
        // SAFETY: the caller meets `dealloc`'s contract, which `System.dealloc` shares. Tested by
        // tests::the_allocator_passes_requests_through.
        unsafe { System.dealloc(block, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: Counting = Counting;

#[cfg(test)]
mod tests {
    use super::*;

    /// The allocator hands every request to the system's unchanged: allocating, zeroing, growing, and freeing.
    #[test]
    fn the_allocator_passes_requests_through() {
        let mut grown: Vec<u64> = Vec::with_capacity(1);
        grown.extend(0..1000);
        assert_eq!(grown.iter().sum::<u64>(), 499_500);
        assert!(vec![0u8; 4096].iter().all(|b| *b == 0));
    }

    /// The count of heap bytes in use rises by a block's size when it is allocated or grown and falls when it is freed,
    /// within what the other tests' threads allocate meanwhile, which is far less than the block.
    #[test]
    fn the_heap_in_use_follows_a_blocks_allocation_growth_and_free() {
        const BLOCK: usize = 8 << 20;
        let near = |measured: usize, expected: usize| measured.abs_diff(expected) < BLOCK / 4;
        let before = heap_in_use();
        let mut block: Vec<u8> = std::hint::black_box(Vec::with_capacity(BLOCK));
        assert!(near(heap_in_use(), before + BLOCK));
        block.reserve_exact(2 * BLOCK);
        assert!(near(heap_in_use(), before + 2 * BLOCK));
        drop(std::hint::black_box(block));
        assert!(near(heap_in_use(), before));
        let zeroed = std::hint::black_box(vec![0u8; BLOCK]);
        assert!(near(heap_in_use(), before + BLOCK));
        drop(zeroed);
        assert!(near(heap_in_use(), before));
    }
}
