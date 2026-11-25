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

//! The C port's values simc reads and makes: paths, bytes, strings, vectors, times, and errors, each made with the C
//! port's own allocator, so the C drops what simc hands it as it drops its own.

use std::alloc::Layout;
use std::ffi::{OsStr, c_char, c_void};
use std::mem::MaybeUninit;
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock, PoisonError};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use host::cache::{self, Bytes, Found, Key, Placement};
use host::delta::directory::{Opened, Owner};
use host::delta::{Page, Size, WordOp};
use host::shared::{Form, Kind, Limits, Looked, Op};
use host::{Error, Snapshot, SnapshotKind, Status, WalKind, Xid};
use sim::{Ending, Outcome};

use crate::ffi;

/// slice reads the C's values of a pointer and a length, none for a zero length.
///
/// # Safety
///
/// A nonzero `len` comes with a `ptr` to `len` values that outlive `'a` and that nothing writes while `'a` lasts.
pub(crate) unsafe fn slice<'a, T>(ptr: *const T, len: usize) -> &'a [T] {
    if len == 0 {
        return &[];
    }
    // SAFETY: the caller's contract, `len` values at `ptr` for `'a`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_key_doors_answer_as_the_simulations_host.
    unsafe { std::slice::from_raw_parts(ptr, len) }
}

/// bytes_mut reads a buffer the C passes as a pointer and a length to fill, none for a zero length.
///
/// # Safety
///
/// A nonzero `len` comes with a `ptr` to `len` writable bytes that outlive `'a` and that nothing else touches while
/// `'a` lasts.
pub(crate) unsafe fn bytes_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if len == 0 {
        return &mut [];
    }
    // SAFETY: the caller's contract, `len` writable bytes at `ptr`, borrowed alone for `'a`. Tested by
    // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { std::slice::from_raw_parts_mut(ptr, len) }
}

/// path reads a path the C passes as the bytes of an `OsStr`.
///
/// # Safety
///
/// `slice`'s contract, and the bytes are an `OsStr`'s encoded bytes, as every C path is: UTF-8 the C wrote, or bytes a
/// Rust `OsStr` gave it.
pub(crate) unsafe fn path<'a>(ptr: *const c_char, len: usize) -> &'a Path {
    // SAFETY: `slice`'s contract is the caller's. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    let encoded = unsafe { slice::<u8>(ptr.cast(), len) };
    // SAFETY: the caller's contract, an `OsStr`'s encoded bytes. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    Path::new(unsafe { OsStr::from_encoded_bytes_unchecked(encoded) })
}

/// FAILING is how many of simc's allocations for the C come before the one a test fails, plus one, or 0 for none.
static FAILING: AtomicU64 = AtomicU64::new(0);

/// fail_allocation fails the `nth` of simc's next allocations for the C, counting from 1, as the C's allocator does
/// when it runs out of memory, so that a test reaches what a failed allocation does.
pub(crate) fn fail_allocation(nth: u64) {
    FAILING.store(nth, Ordering::Relaxed);
}

/// allocated returns a block of `size` bytes at `align` from the C's allocator, or null when it has none or a test
/// fails this allocation; it never raises in the C.
pub(crate) fn allocated(size: usize, align: usize) -> *mut c_void {
    if FAILING.try_update(Ordering::Relaxed, Ordering::Relaxed, |left| left.checked_sub(1)) == Ok(1) {
        return std::ptr::null_mut();
    }
    // SAFETY: `std_global_alloc` returns a block of `size` bytes at `align`, or null. Tested by
    // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { ffi::std_global_alloc(size, align) }
}

/// placement reads a C `Placement`.
pub(crate) fn placement(placement: ffi::HostCachePlacement) -> Placement {
    match placement {
        ffi::HostCachePlacement_HOST_CACHE_PLACEMENT_SERVER => Placement::Server,
        ffi::HostCachePlacement_HOST_CACHE_PLACEMENT_BACKEND => Placement::Backend,
        other => unreachable!("the C's placements are the Rust's two, never {other}"),
    }
}

/// key reads a C `Key` as the Rust's, made by `cache::key` of the location, address, and locator the C's holds.
pub(crate) fn key(key: &ffi::HostCacheKey) -> Key {
    let (location, rest) = key.f0.split_first_chunk::<8>().expect("a key starts with its packs' location");
    let (address, locator) = rest.split_first_chunk::<{ cache::ADDRESS }>().expect("a key's address follows");
    let locator = locator.try_into().expect("a key ends with its locator");
    cache::key(u64::from_le_bytes(*location), address, locator)
}

/// found makes a C `Found` of a frame the simulation pinned: its number and its bytes, a lent frame's copied into an
/// `Arc<[u8]>` of the C's allocator, since the C frees a lent frame's bytes with its own allocator.
pub(crate) fn found(found: &Found) -> ffi::HostCacheFound {
    ffi::HostCacheFound { frame: found.frame, bytes: cache_bytes(&found.bytes) }
}

/// cache_bytes makes C `Bytes` of the Rust's, lent bytes copied into an `Arc<[u8]>` of the C's allocator, since the C
/// frees lent bytes with its own allocator.
pub(crate) fn cache_bytes(bytes: &Bytes) -> ffi::HostCacheBytes {
    match bytes {
        Bytes::Mapped(mapped) => ffi::HostCacheBytes {
            tag: ffi::HostCacheBytesTag_HOST_CACHE_BYTES_MAPPED,
            u: ffi::HostCacheBytes__bindgen_ty_1 {
                mapped: ffi::HostCacheBytes__bindgen_ty_1__bindgen_ty_1 { f0: mapped.as_ptr(), f0_len: mapped.len() },
            },
        },
        Bytes::Lent(lent) => ffi::HostCacheBytes {
            tag: ffi::HostCacheBytesTag_HOST_CACHE_BYTES_LENT,
            u: ffi::HostCacheBytes__bindgen_ty_1 {
                lent: ffi::HostCacheBytes__bindgen_ty_1__bindgen_ty_2 { f0: arc(lent), f0_len: lent.len() },
            },
        },
    }
}

/// arc makes a C `Arc<[u8]>` of bytes, its block laid out as Rust's `ArcInner` and allocated by `allocated`, a failed
/// allocation panicking with `OutOfMemory`.
fn arc(bytes: &[u8]) -> *mut ffi::StdArcInner {
    let counts = Layout::new::<[usize; 2]>();
    let (layout, offset) = counts
        .extend(Layout::array::<u8>(bytes.len()).expect("a frame's bytes fit the address space"))
        .expect("a frame's Arc fits the address space");
    let layout = layout.pad_to_align();
    let block = allocated(layout.size(), layout.align());
    if block.is_null() {
        std::panic::panic_any(crate::entry::OutOfMemory);
    }
    // SAFETY: `block` is a fresh block of `ArcInner<[u8]>`'s layout for the bytes, its counts first and its bytes at
    // `offset`, which they can't overlap. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
    unsafe {
        block.cast::<[usize; 2]>().write([1, 1]);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), block.cast::<u8>().add(offset), bytes.len());
    }
    block.cast()
}

/// Filling is a C fill's call: the fill, its captures, the bytes it writes, and the backend's state its entry found.
pub(crate) struct Filling {
    /// fill is the C's fill.
    pub(crate) fill: unsafe extern "C" fn(*mut c_void, *mut u8, usize),
    /// captures are the fill's captures.
    pub(crate) captures: *mut c_void,
    /// bytes are the bytes the fill writes.
    pub(crate) bytes: *mut u8,
    /// len is how many bytes the fill writes.
    pub(crate) len: usize,
    /// state is the backend's state the fill's entry found, its actor's own, which a yield before the fill replaced.
    pub(crate) state: *const ffi::SimcBackend,
}

/// fill_call calls a C fill inside the C's catch, holding nothing to drop, so a C panic in the fill unwinds to the
/// catch past this frame.
///
/// # Safety
///
/// `filling` is a `Filling` whose fill may be called with its captures and bytes.
unsafe extern "C" fn fill_call(filling: *mut c_void) {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
    unsafe {
        let filling = &*filling.cast::<Filling>();
        (filling.fill)(filling.captures, filling.bytes, filling.len);
    }
}

/// filled calls a C fill under its entry's own backend state, and resumes a C panic in it as a Rust unwind carrying
/// the C's payload, which the entry raises in the C unchanged.
///
/// # Safety
///
/// `filling`'s fill may be called once with its captures and bytes, and its state is its entry's.
pub(crate) unsafe fn filled(filling: &mut Filling) {
    let mut caught = MaybeUninit::<ffi::StdCaught>::uninit();
    // SAFETY: the caller's contract, the state the fill's entry saved on this thread. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state.
    unsafe { ffi::simc_backend_restore(filling.state) };
    // SAFETY: `std_catch_unwind` calls `fill_call` with `filling`, as the caller's contract allows, reports whether it
    // returned, and writes the payload of a panic it caught. Tested by test/t/106_cport_simc.pl:
    // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame; test/t/106_cport_simc.pl:
    // simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload.
    let returned = unsafe { ffi::std_catch_unwind(Some(fill_call), (&raw mut *filling).cast(), caught.as_mut_ptr()) };
    if !returned {
        // SAFETY: the catch wrote the caught payload. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload.
        std::panic::resume_unwind(Box::new(crate::entry::Caught(unsafe { caught.assume_init() })));
    }
}

/// DANGLING is the C's pointer of a vector that holds no block, its `STD_DANGLING`.
const DANGLING: *mut c_void = std::ptr::without_provenance_mut(ffi::MAXIMUM_ALIGNOF as usize);

/// copied makes a C vector of `len` elements of `size` bytes copied from `elements`, its block from the C's allocator
/// through `allocated`, which returns null rather than raising, so no C unwinding starts while simc holds Rust
/// values; a failed allocation panics with `OutOfMemory`, which the entry raises in the C as the C's own.
fn copied(elements: *const u8, len: usize, size: usize, align: usize) -> ffi::StdVec {
    let bytes = len.checked_mul(size).expect("a vector simc copies fits the address space, as its source does");
    if bytes == 0 {
        return ffi::StdVec { ptr: DANGLING, len, cap: 0 };
    }
    let block = allocated(bytes, align);
    if block.is_null() {
        std::panic::panic_any(crate::entry::OutOfMemory);
    }
    // SAFETY: the caller's `elements` are `bytes` initialized bytes, and the fresh block, which they can't overlap,
    // holds as many. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { std::ptr::copy_nonoverlapping(elements, block.cast::<u8>(), bytes) };
    ffi::StdVec { ptr: block, len, cap: len }
}

/// vec makes a C vector of bytes.
pub(crate) fn vec(bytes: &[u8]) -> ffi::StdVec {
    copied(bytes.as_ptr(), bytes.len(), 1, 1)
}

/// string makes a C `String`.
pub(crate) fn string(text: &str) -> ffi::StdString {
    vec(text.as_bytes())
}

/// Made are C byte vectors simc made, a vector's strings or slots, which it frees should it fail before the C
/// takes them.
struct Made(Vec<ffi::StdVec>);

impl Drop for Made {
    /// drop frees the byte vectors the C never took.
    fn drop(&mut self) {
        for made in &self.0 {
            if made.cap != 0 {
                // SAFETY: each vector's block is one `copied` made at alignment 1 with `cap` bytes, which nothing else
                // holds. Tested by test/t/106_cport_simc.pl:
                // simc::tests::a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory.
                unsafe { ffi::std_global_dealloc(made.ptr, made.cap, 1) };
            }
        }
    }
}

/// vecs makes a C vector of byte vectors, each made before the vector that holds them.
pub(crate) fn vecs<T: AsRef<[u8]>>(items: &[T]) -> ffi::StdVec {
    let mut made = Made(Vec::with_capacity(items.len()));
    for item in items {
        made.0.push(vec(item.as_ref()));
    }
    let list = copied(made.0.as_ptr().cast(), made.0.len(), size_of::<ffi::StdVec>(), align_of::<ffi::StdVec>());
    made.0.clear();
    list
}

/// strings makes a C vector of `String`s, each string made before the vector that holds them.
pub(crate) fn strings(texts: &[String]) -> ffi::StdVec {
    vecs(texts)
}

/// logged makes a C vector of the records the simulation's WAL took, each body made before the vector that holds them.
pub(crate) fn logged(records: &[(u8, Vec<u8>, host::Lsn)]) -> ffi::StdVec {
    let mut made = Made(Vec::with_capacity(records.len()));
    let mut tuples = Vec::with_capacity(records.len());
    for (kind, body, end) in records {
        let f1 = vec(body);
        made.0.push(f1);
        tuples.push(ffi::simc_TupleU8VecU8Lsn_t { f0: *kind, f1, f2: *end });
    }
    let (size, align) = (size_of::<ffi::simc_TupleU8VecU8Lsn_t>(), align_of::<ffi::simc_TupleU8VecU8Lsn_t>());
    let list = copied(tuples.as_ptr().cast(), tuples.len(), size, align);
    made.0.clear();
    list
}

/// new_cids makes a C vector of the new cid records the simulation's WAL took.
pub(crate) fn new_cids(cids: &[(host::Storage, host::Row, host::NewCid, host::Lsn)]) -> ffi::StdVec {
    let command =
        |command: Option<host::Command>| (command.is_some(), ffi::HostCommand { f0: command.map_or(0, |c| c.0) });
    let made: Vec<ffi::simc_TupleStorageRowNewCidLsn_t> = cids
        .iter()
        .map(|(storage, row, cid, end)| {
            let ((has_cmin, cmin), (has_cmax, cmax), (has_combo, combo)) =
                (command(cid.cmin), command(cid.cmax), command(cid.combo));
            ffi::simc_TupleStorageRowNewCidLsn_t {
                f0: ffi::HostStorage {
                    tablespace: storage.tablespace,
                    database: storage.database,
                    relation: storage.relation,
                },
                f1: ffi::HostRow { f0: row.0 },
                f2: ffi::HostNewCid { has_cmin, cmin, has_cmax, cmax, has_combo, combo },
                f3: *end,
            }
        })
        .collect();
    let (size, align) =
        (size_of::<ffi::simc_TupleStorageRowNewCidLsn_t>(), align_of::<ffi::simc_TupleStorageRowNewCidLsn_t>());
    copied(made.as_ptr().cast(), made.len(), size, align)
}

/// pairs makes a C vector of `(u64, u64)` pairs, a sweep's.
pub(crate) fn pairs(pairs: &[(u64, u64)]) -> ffi::StdVec {
    let made: Vec<ffi::std_TupleU64U64_t> = pairs.iter().map(|&(f0, f1)| ffi::std_TupleU64U64_t { f0, f1 }).collect();
    copied(made.as_ptr().cast(), made.len(), size_of::<ffi::std_TupleU64U64_t>(), align_of::<ffi::std_TupleU64U64_t>())
}

/// numbers makes a C vector of `u32`s, a list of processes.
pub(crate) fn numbers(numbers: &[u32]) -> ffi::StdVec {
    copied(numbers.as_ptr().cast(), numbers.len(), size_of::<u32>(), align_of::<u32>())
}

/// indexes makes a C vector of `usize`s, a run's trace.
pub(crate) fn indexes(indexes: &[usize]) -> ffi::StdVec {
    copied(indexes.as_ptr().cast(), indexes.len(), size_of::<usize>(), align_of::<usize>())
}

/// op reads a C `Op` as the Rust's.
pub(crate) fn op(op: ffi::HostSharedOp) -> Op {
    match op.tag {
        // SAFETY: an add's tag names its union's `add`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
        ffi::HostSharedOpTag_HOST_SHARED_OP_ADD => Op::Add(unsafe { op.u.add }),
        ffi::HostSharedOpTag_HOST_SHARED_OP_EXCHANGE => {
            // SAFETY: an exchange's tag names its union's `exchange`. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            let exchange = unsafe { op.u.exchange };
            Op::Exchange(exchange.f0, exchange.f1)
        }
        _ => Op::Load,
    }
}

/// page reads a C delta page's name as the Rust's.
pub(crate) fn page(page: ffi::HostDeltaPagePage) -> Page {
    Page::named(page.f0)
}

/// size reads a C delta page's size as the Rust's.
pub(crate) fn size(size: ffi::HostDeltaPageSize) -> Size {
    Size { bytes: size.bytes, words: size.words }
}

/// opened reads a C opened page as the Rust's.
pub(crate) fn opened(opened: &ffi::HostDeltaDirectoryOpened) -> Opened {
    Opened { slot: opened.slot, body: opened.body, size: size(opened.size) }
}

/// opened_page makes a C opened page of the Rust's.
pub(crate) fn opened_page(opened: Opened) -> ffi::HostDeltaDirectoryOpened {
    let size = ffi::HostDeltaPageSize { bytes: opened.size.bytes, words: opened.size.words };
    ffi::HostDeltaDirectoryOpened { slot: opened.slot, body: opened.body, size }
}

/// owner reads a C page's owner as the Rust's.
pub(crate) fn owner(owner: ffi::HostDeltaDirectoryOwner) -> Owner {
    Owner { shape: owner.shape, location: owner.location, lock: owner.lock }
}

/// word_op reads a C `WordOp` as the Rust's.
pub(crate) fn word_op(op: ffi::HostDeltaWordOp) -> WordOp {
    match op.tag {
        // SAFETY: a store's tag names its union's `store`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
        ffi::HostDeltaWordOpTag_HOST_DELTA_WORD_OP_STORE => WordOp::Store(unsafe { op.u.store }),
        ffi::HostDeltaWordOpTag_HOST_DELTA_WORD_OP_EXCHANGE => {
            // SAFETY: an exchange's tag names its union's `exchange`. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            let exchange = unsafe { op.u.exchange };
            WordOp::Exchange(exchange.f0, exchange.f1)
        }
        // SAFETY: an add's tag names its union's `add`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
        ffi::HostDeltaWordOpTag_HOST_DELTA_WORD_OP_ADD => WordOp::Add(unsafe { op.u.add }),
        _ => WordOp::Load,
    }
}

/// status makes a C `Status` of the Rust's.
pub(crate) fn status(status: Status) -> ffi::HostStatus {
    match status {
        Status::InProgress => ffi::HostStatus_HOST_STATUS_IN_PROGRESS,
        Status::Committed => ffi::HostStatus_HOST_STATUS_COMMITTED,
        Status::Aborted => ffi::HostStatus_HOST_STATUS_ABORTED,
    }
}

/// xids reads the C's xids as the Rust's, none for a zero length.
///
/// # Safety
///
/// A nonzero `len` comes with `ptr` to `len` xids for the call.
pub(crate) unsafe fn xids(ptr: *const ffi::HostXid, len: usize) -> Vec<Xid> {
    let held: &[ffi::HostXid] = match len {
        0 => &[],
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids.
        len => unsafe { std::slice::from_raw_parts(ptr, len) },
    };
    held.iter().map(|xid| Xid(xid.f0)).collect()
}

/// snapshot makes a C snapshot of a lent one, its xids lent from the Rust's, and its own snapshot the `Taken` at `of`,
/// of `TAKEN`'s type.
pub(crate) fn snapshot(lent: &Snapshot<'_>, of: *mut c_void) -> ffi::HostSnapshot {
    let kind = match lent.kind {
        SnapshotKind::Mvcc => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_MVCC,
        SnapshotKind::Current => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_CURRENT,
        SnapshotKind::Any => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_ANY,
        SnapshotKind::Toast => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_TOAST,
        SnapshotKind::Dirty => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_DIRTY,
        SnapshotKind::HistoricMvcc => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_HISTORIC_MVCC,
        SnapshotKind::NonVacuumable => ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_NON_VACUUMABLE,
    };
    ffi::HostSnapshot {
        kind,
        xmin: ffi::HostXid { f0: lent.xmin.0 },
        xmax: ffi::HostXid { f0: lent.xmax.0 },
        command: ffi::HostCommand { f0: lent.command.0 },
        recovery: lent.recovery,
        overflowed: lent.overflowed,
        xip: lent.xip.as_ptr(),
        xip_len: lent.xip.len(),
        subxip: lent.subxip.as_ptr(),
        subxip_len: lent.subxip.len(),
        of: ffi::StdAny { type_: &TAKEN.0, value: of },
    }
}

/// pages_into lists pages' names in the room the C reserved in a vector of them, past its length.
///
/// # Safety
///
/// `names` is a C vector of page names with room for every page past its length.
pub(crate) unsafe fn pages_into(names: *mut ffi::StdVec, pages: &[Page]) {
    // SAFETY: the caller's contract, a vector with room past its length for every page. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host.
    unsafe {
        let names = &mut *names;
        let slots = names.ptr.cast::<ffi::HostDeltaPagePage>().add(names.len);
        for (at, page) in pages.iter().enumerate() {
            slots.add(at).write(ffi::HostDeltaPagePage { f0: page.value() });
        }
        names.len += pages.len();
    }
}

/// looked makes a C `Looked`.
pub(crate) fn looked(looked: Looked) -> ffi::HostSharedLooked {
    let (tag, mine) = match looked {
        Looked::Mine(place) => (ffi::HostSharedLookedTag_HOST_SHARED_LOOKED_MINE, place),
        Looked::Theirs => (ffi::HostSharedLookedTag_HOST_SHARED_LOOKED_THEIRS, 0),
        Looked::Used => (ffi::HostSharedLookedTag_HOST_SHARED_LOOKED_USED, 0),
    };
    ffi::HostSharedLooked { tag, u: ffi::HostSharedLooked__bindgen_ty_1 { mine } }
}

/// wal_kind reads a C WAL record's kind as the host's, by its name; a kind the host's build has no form of, as the
/// injection points' test record in a build without them, is a named internal error.
pub(crate) fn wal_kind(kind: ffi::HostWalKind) -> WalKind {
    match kind {
        ffi::HostWalKind_HOST_WAL_KIND_REPOSITORY_CREATED => WalKind::RepositoryCreated,
        ffi::HostWalKind_HOST_WAL_KIND_REPOSITORY_DROPPED => WalKind::RepositoryDropped,
        ffi::HostWalKind_HOST_WAL_KIND_REPOSITORY_MOVED => WalKind::RepositoryMoved,
        ffi::HostWalKind_HOST_WAL_KIND_ROW_CHANGE => WalKind::RowChange,
        ffi::HostWalKind_HOST_WAL_KIND_PAGE_LEFT => WalKind::PageLeft,
        ffi::HostWalKind_HOST_WAL_KIND_CHUNK => WalKind::Chunk,
        ffi::HostWalKind_HOST_WAL_KIND_BASE_ADVANCED => WalKind::BaseAdvanced,
        ffi::HostWalKind_HOST_WAL_KIND_STORAGE_EMPTIED => WalKind::StorageEmptied,
        ffi::HostWalKind_HOST_WAL_KIND_CONFIRM => WalKind::Confirm,
        ffi::HostWalKind_HOST_WAL_KIND_TIDS_FREED => WalKind::TidsFreed,
        ffi::HostWalKind_HOST_WAL_KIND_MAP_CARRIED => WalKind::MapCarried,
        ffi::HostWalKind_HOST_WAL_KIND_DATABASE_CHANGE => WalKind::DatabaseChange,
        ffi::HostWalKind_HOST_WAL_KIND_CARRY_UNLISTED => WalKind::CarryUnlisted,
        ffi::HostWalKind_HOST_WAL_KIND_FOLD_STATUSES => WalKind::FoldStatuses,
        other => panic!("internal error: the simulation's host has no WAL record kind {other}"),
    }
}

/// path_buf makes a C `PathBuf` of a path, as `to_path_buf` does.
pub(crate) fn path_buf(path: &Path) -> ffi::StdPathBuf {
    let bytes = vec(path.as_os_str().as_encoded_bytes());
    #[cfg(windows)]
    let made = ffi::StdPathBuf { bytes, is_known_utf8: false };
    #[cfg(not(windows))]
    let made = ffi::StdPathBuf { bytes };
    made
}

/// error makes a C `host::Error` of a fork error without a hint, the only kind the simulation's host returns.
pub(crate) fn error(error: Error) -> ffi::HostError {
    let (sqlstate, message, hint) = error.into_parts();
    assert!(hint.is_none(), "the simulation's host makes no hinted errors");
    let mut text = string(&message);
    // SAFETY: `host_Error_fork` takes the SQLSTATE's five bytes and the message, which it moves out of `text`. Tested
    // by test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { ffi::host_Error_fork(sqlstate.as_ptr(), &mut text) }
}

/// error_from takes a C `host::Error`, the fork's own, as the Rust's: an unhinted one by its message, borrowed or
/// owned, and a hinted one, which the C makes only as `host_Error_hinted` does, of its string literals.
///
/// # Safety
///
/// `error` points to a C error simc may take, which it drops; a borrowed message, and a hint, are the C's string
/// literals.
pub(crate) unsafe fn error_from(error: *mut ffi::HostError) -> Error {
    // SAFETY: the caller's contract, a live C error. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
    let held = unsafe { &*error };
    assert_eq!(held.cause.tag, ffi::HostCauseTag_HOST_CAUSE_FORK, "only `pghost` makes Postgres' errors");
    // SAFETY: a fork cause's union field is `fork`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
    let fork = unsafe { &held.cause.u.fork };
    let borrowed = fork.f0.tag == ffi::StdCowStrTag_STD_COW_STR_BORROWED;
    let made = match (borrowed, fork.f1.ptr.is_null()) {
        (true, false) => {
            // SAFETY: the caller's contract, a hinted error's message and hint the C's string literals, which live as
            // long as the module. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
            let (message, hint) = unsafe { (literal(fork.f0.u.borrowed), literal(fork.f1)) };
            Error::hinted(&held.sqlstate, message, hint)
        }
        (false, false) => panic!("a hinted error's message is one of the C's literals, as host_Error_hinted makes it"),
        (true, true) => {
            // SAFETY: the caller's contract, a borrowed message's UTF-8 bytes, which the error holds. Tested by
            // test/t/106_cport_simc.pl: simc::tests::an_unhinted_c_error_crosses_to_sim_as_its_message.
            let text = unsafe { slice::<u8>(fork.f0.u.borrowed.ptr.cast(), fork.f0.u.borrowed.len) };
            Error::fork(&held.sqlstate, String::from_utf8_lossy(text).into_owned())
        }
        (false, true) => {
            // SAFETY: the caller's contract, an owned message's UTF-8 bytes in its `String`, which the error holds.
            // Tested by test/t/106_cport_simc.pl: simc::tests::an_unhinted_c_error_crosses_to_sim_as_its_message.
            let text = unsafe { slice::<u8>(fork.f0.u.owned.ptr.cast(), fork.f0.u.owned.len) };
            Error::fork(&held.sqlstate, String::from_utf8_lossy(text).into_owned())
        }
    };
    // SAFETY: the caller's contract lets simc drop the error, whose text was copied or is static. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
    unsafe { ffi::host_Error_drop(error) };
    made
}

/// literal reads a C string literal as a `'static` str.
///
/// # Safety
///
/// The str is a string literal of the C's, UTF-8 that lives as long as the module.
unsafe fn literal(text: ffi::StdStr) -> &'static str {
    // SAFETY: the caller's contract, a literal's bytes for the module's life. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
    let bytes: &'static [u8] = unsafe { slice::<u8>(text.ptr.cast(), text.len) };
    std::str::from_utf8(bytes).expect("the C's string literals are UTF-8")
}

/// duration makes a C `Duration`.
pub(crate) fn duration(duration: Duration) -> ffi::StdDuration {
    ffi::StdDuration { secs: duration.as_secs(), nanos: duration.subsec_nanos() }
}

/// duration_from reads a C `Duration`.
pub(crate) fn duration_from(duration: ffi::StdDuration) -> Duration {
    Duration::new(duration.secs, duration.nanos)
}

/// system_time makes a C `SystemTime` of a time, as far from the C's epoch as it is from Rust's.
pub(crate) fn system_time(time: SystemTime) -> ffi::StdSystemTime {
    let mut made = MaybeUninit::<ffi::StdSystemTime>::uninit();
    // SAFETY: `STD_UNIX_EPOCH` is the C's constant epoch, and each checked step writes its result at its place when it
    // returns true. Tested by test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
    let fits = unsafe {
        match time.duration_since(UNIX_EPOCH) {
            Ok(after) => ffi::std_SystemTime_checked_add(&ffi::STD_UNIX_EPOCH, duration(after), made.as_mut_ptr()),
            Err(before) => {
                ffi::std_SystemTime_checked_sub(&ffi::STD_UNIX_EPOCH, duration(before.duration()), made.as_mut_ptr())
            }
        }
    };
    assert!(fits, "a time the simulation's clock gives fits the C's SystemTime, as it fits Rust's");
    // SAFETY: the checked step returned true, so it wrote the time. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { made.assume_init() }
}

/// BASE is the instant of simc's install of the simulation's host, with the C's own reading of its clock then; every
/// instant the simulation gives later follows it, its clock starting from the real one, so each is offset from the
/// C's reading by as much as it is from the Rust's.
static BASE: OnceLock<(Instant, ffi::StdInstant)> = OnceLock::new();

/// instant_base takes simc's base instants, at the install of the simulation's host: the simulation's clock, the real
/// one while no simulation runs, and the C's.
pub(crate) fn instant_base() {
    // SAFETY: `std_Instant_now` reads the C's monotonic clock. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    BASE.get_or_init(|| (host::Host::instant(&sim::SimHost), unsafe { ffi::std_Instant_now() }));
}

/// instant makes a C `Instant` of an instant, as far past simc's base in the C as it is in Rust, through the C's own
/// `checked_add`, so the C's layout of an instant, which differs by platform, stays the C's.
pub(crate) fn instant(instant: Instant) -> ffi::StdInstant {
    let (base, base_c) = BASE.get().expect("simc takes its base instants when it installs the host");
    let after = instant.checked_duration_since(*base).expect("an instant the simulation gives follows simc's install");
    let mut made = MaybeUninit::<ffi::StdInstant>::uninit();
    // SAFETY: `std_Instant_checked_add` writes the sum at its place when it returns true. Tested by
    // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
    let fits = unsafe { ffi::std_Instant_checked_add(base_c, duration(after), made.as_mut_ptr()) };
    assert!(fits, "an instant the simulation's clock gives fits the C's Instant, as it fits Rust's");
    // SAFETY: the sum fit, so it was written. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { made.assume_init() }
}

/// KINDS are `io::ErrorKind`'s variants by their names, in core's order, which the C's `StdErrorKind` keeps.
const KINDS: [&str; 44] = [
    "NotFound",
    "PermissionDenied",
    "ConnectionRefused",
    "ConnectionReset",
    "HostUnreachable",
    "NetworkUnreachable",
    "ConnectionAborted",
    "NotConnected",
    "AddrInUse",
    "AddrNotAvailable",
    "NetworkDown",
    "BrokenPipe",
    "AlreadyExists",
    "WouldBlock",
    "NotADirectory",
    "IsADirectory",
    "DirectoryNotEmpty",
    "ReadOnlyFilesystem",
    "FilesystemLoop",
    "StaleNetworkFileHandle",
    "InvalidInput",
    "InvalidData",
    "TimedOut",
    "WriteZero",
    "StorageFull",
    "NotSeekable",
    "QuotaExceeded",
    "FileTooLarge",
    "ResourceBusy",
    "ExecutableFileBusy",
    "Deadlock",
    "CrossesDevices",
    "TooManyLinks",
    "InvalidFilename",
    "ArgumentListTooLong",
    "Interrupted",
    "Unsupported",
    "UnexpectedEof",
    "OutOfMemory",
    "InProgress",
    "TooManyOpenFiles",
    "InputOutputError",
    "Other",
    "Uncategorized",
];

/// io_error makes a C `io::Error`: an operating system's error by its code, as the C's `from_raw_os_error` makes one,
/// and any other by its kind, the C's `Simple` form, its bit-packed kind above `TAG_SIMPLE`.
pub(crate) fn io_error(error: &std::io::Error) -> ffi::StdIoError {
    if let Some(code) = error.raw_os_error() {
        // SAFETY: `std_io_Error_from_raw_os_error` packs a code into an error without allocating. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error.
        return unsafe { ffi::std_io_Error_from_raw_os_error(code) };
    }
    let name = format!("{:?}", error.kind());
    let kind = KINDS.iter().position(|known| *known == name).expect("io::ErrorKind's variants are core's");
    ffi::StdIoError { repr: (kind << 32) | 0x3 }
}

/// caught_message returns a caught panic's message, its `&str` or `String`, or nothing for another payload, as sim's
/// runner reads an actor's panic, and drops the payload.
///
/// # Safety
///
/// `caught` is a payload the C's catch wrote, which this drops.
pub(crate) unsafe fn caught_message(caught: *mut ffi::StdCaught) -> String {
    let (mut text, mut len) = (std::ptr::null(), 0);
    // SAFETY: the caller's contract; a downcast writes the message's bytes, which the payload holds until its drop.
    // Tested by test/t/106_cport_simc.pl: simc::tests::an_actors_c_panic_fails_its_run_with_its_message.
    let message = unsafe {
        if ffi::std_Caught_downcast_string(caught, &mut text, &mut len)
            || ffi::std_Caught_downcast_str(caught, &mut text, &mut len)
        {
            String::from_utf8_lossy(slice::<u8>(text.cast(), len)).into_owned()
        } else {
            String::new()
        }
    };
    // SAFETY: the caller's contract, the payload's drop. Tested by test/t/106_cport_simc.pl:
    // simc::tests::an_actors_c_panic_fails_its_run_with_its_message.
    unsafe { ffi::std_Caught_drop(caught) };
    message
}

/// ending makes a C `Ending`.
fn ending(ending: &Ending) -> ffi::SimcEnding {
    match ending {
        Ending::Terminated => ffi::SimcEnding {
            tag: ffi::SimcEndingTag_SIMC_ENDING_TERMINATED,
            is_err: false,
            sqlstate: [0; 5],
            message: vec(&[]),
        },
        Ending::Returned(Ok(())) => ffi::SimcEnding {
            tag: ffi::SimcEndingTag_SIMC_ENDING_RETURNED,
            is_err: false,
            sqlstate: [0; 5],
            message: vec(&[]),
        },
        Ending::Returned(Err((sqlstate, message))) => ffi::SimcEnding {
            tag: ffi::SimcEndingTag_SIMC_ENDING_RETURNED,
            is_err: true,
            sqlstate: *sqlstate,
            message: string(message),
        },
    }
}

/// ending_from reads a C `Ending` as the Rust's.
pub(crate) fn ending_from(ending: &ffi::SimcEnding) -> Ending {
    if ending.tag == ffi::SimcEndingTag_SIMC_ENDING_TERMINATED {
        return Ending::Terminated;
    }
    if !ending.is_err {
        return Ending::Returned(Ok(()));
    }
    // SAFETY: an erring ending's message is a C `String` simc made, of its length's UTF-8 bytes. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    let message = unsafe { slice::<u8>(ending.message.ptr.cast(), ending.message.len) };
    Ending::Returned(Err((ending.sqlstate, String::from_utf8_lossy(message).into_owned())))
}

/// outcome makes a C `Outcome`.
pub(crate) fn outcome(outcome: &Outcome) -> ffi::SimcOutcome {
    match outcome {
        Outcome::Ended(endings) => {
            let mut made = Made(Vec::with_capacity(2 * endings.len()));
            let mut tuples = Vec::with_capacity(endings.len());
            for (name, end) in endings {
                let f0 = string(name);
                made.0.push(f0);
                let f1 = ending(end);
                made.0.push(f1.message);
                tuples.push(ffi::simc_TupleStringEnding_t { f0, f1 });
            }
            let size = size_of::<ffi::simc_TupleStringEnding_t>();
            let list = copied(tuples.as_ptr().cast(), tuples.len(), size, align_of::<ffi::simc_TupleStringEnding_t>());
            made.0.clear();
            ffi::SimcOutcome {
                tag: ffi::SimcOutcomeTag_SIMC_OUTCOME_ENDED,
                u: ffi::SimcOutcome__bindgen_ty_1 { ended: list },
            }
        }
        Outcome::Crashed => ffi::SimcOutcome {
            tag: ffi::SimcOutcomeTag_SIMC_OUTCOME_CRASHED,
            u: ffi::SimcOutcome__bindgen_ty_1 { ended: vec(&[]) },
        },
        Outcome::Failed(message) => ffi::SimcOutcome {
            tag: ffi::SimcOutcomeTag_SIMC_OUTCOME_FAILED,
            u: ffi::SimcOutcome__bindgen_ty_1 { failed: string(message) },
        },
    }
}

/// outcome_from reads a C `Outcome` as the Rust's.
///
/// # Safety
///
/// `outcome` is an outcome `outcome` made.
pub(crate) unsafe fn outcome_from(outcome: &ffi::SimcOutcome) -> Outcome {
    // SAFETY: the caller's contract; the tag names the union's field, and an ended outcome's vector holds its length's
    // tuples. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    unsafe {
        match outcome.tag {
            ffi::SimcOutcomeTag_SIMC_OUTCOME_ENDED => {
                let list = &outcome.u.ended;
                let endings: &[ffi::simc_TupleStringEnding_t] = match list.len {
                    0 => &[],
                    len => std::slice::from_raw_parts(list.ptr.cast(), len),
                };
                Outcome::Ended(
                    endings
                        .iter()
                        .map(|tuple| {
                            let name = slice::<u8>(tuple.f0.ptr.cast(), tuple.f0.len);
                            (String::from_utf8_lossy(name).into_owned(), ending_from(&tuple.f1))
                        })
                        .collect(),
                )
            }
            ffi::SimcOutcomeTag_SIMC_OUTCOME_CRASHED => Outcome::Crashed,
            _ => {
                let message = slice::<u8>(outcome.u.failed.ptr.cast(), outcome.u.failed.len);
                Outcome::Failed(String::from_utf8_lossy(message).into_owned())
            }
        }
    }
}

/// freed frees a C vector's block simc made of `size`-byte elements at `align`.
///
/// # Safety
///
/// `list` is a vector `copied` made, freed once.
unsafe fn freed(list: &ffi::StdVec, size: usize, align: usize) {
    if list.cap != 0 {
        // SAFETY: the caller's contract, a block of `cap` elements `copied` allocated. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        unsafe { ffi::std_global_dealloc(list.ptr, list.cap * size, align) };
    }
}

/// outcome_drop frees a C `Outcome` simc made.
///
/// # Safety
///
/// `outcome` is an outcome `outcome` made, freed once.
pub(crate) unsafe fn outcome_drop(outcome: &mut ffi::SimcOutcome) {
    // SAFETY: the caller's contract; the tag names the union's field, and each string and vector is one simc made.
    // Tested by test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    unsafe {
        match outcome.tag {
            ffi::SimcOutcomeTag_SIMC_OUTCOME_FAILED => freed(&outcome.u.failed, 1, 1),
            _ => {
                let list = &outcome.u.ended;
                if list.len != 0 {
                    let tuples = std::slice::from_raw_parts(list.ptr.cast::<ffi::simc_TupleStringEnding_t>(), list.len);
                    for tuple in tuples {
                        freed(&tuple.f0, 1, 1);
                        freed(&tuple.f1.message, 1, 1);
                    }
                }
                let (size, align) =
                    (size_of::<ffi::simc_TupleStringEnding_t>(), align_of::<ffi::simc_TupleStringEnding_t>());
                freed(list, size, align);
            }
        }
    }
}

/// exits makes a C vector of a run's failed exit works, each string made before the vector that holds them.
pub(crate) fn exits(exits: &[(String, [u8; 5], String)]) -> ffi::StdVec {
    let mut made = Made(Vec::with_capacity(2 * exits.len()));
    let mut tuples = Vec::with_capacity(exits.len());
    for (name, sqlstate, message) in exits {
        let f0 = string(name);
        made.0.push(f0);
        let f2 = string(message);
        made.0.push(f2);
        tuples.push(ffi::simc_TupleStringSqlstateString_t { f0, f1: *sqlstate, f2 });
    }
    let (size, align) =
        (size_of::<ffi::simc_TupleStringSqlstateString_t>(), align_of::<ffi::simc_TupleStringSqlstateString_t>());
    let list = copied(tuples.as_ptr().cast(), tuples.len(), size, align);
    made.0.clear();
    list
}

/// exits_drop frees a C vector `exits` made, its strings first.
///
/// # Safety
///
/// `exits` is a vector `exits` made, freed once.
pub(crate) unsafe fn exits_drop(exits: &mut ffi::StdVec) {
    // SAFETY: the caller's contract; the vector holds its length's tuples, each string one simc made. Tested by
    // test/t/106_cport_simc.pl: simc::tests::each_failing_exit_work_is_in_the_report.
    unsafe {
        if exits.len != 0 {
            let tuples =
                std::slice::from_raw_parts(exits.ptr.cast::<ffi::simc_TupleStringSqlstateString_t>(), exits.len);
            for tuple in tuples {
                freed(&tuple.f0, 1, 1);
                freed(&tuple.f2, 1, 1);
            }
        }
        let (size, align) =
            (size_of::<ffi::simc_TupleStringSqlstateString_t>(), align_of::<ffi::simc_TupleStringSqlstateString_t>());
        freed(exits, size, align);
    }
}

/// warnings makes a C vector of the reports at WARNING the work made, each string made before the vector that holds
/// them.
pub(crate) fn warnings(warnings: &[([u8; 5], String)]) -> ffi::StdVec {
    let mut made = Made(Vec::with_capacity(warnings.len()));
    let mut tuples = Vec::with_capacity(warnings.len());
    for (sqlstate, message) in warnings {
        let f1 = string(message);
        made.0.push(f1);
        tuples.push(ffi::simc_TupleSqlstateString_t { f0: *sqlstate, f1 });
    }
    let (size, align) = (size_of::<ffi::simc_TupleSqlstateString_t>(), align_of::<ffi::simc_TupleSqlstateString_t>());
    let list = copied(tuples.as_ptr().cast(), tuples.len(), size, align);
    made.0.clear();
    list
}

/// TypeId is a C `StdTypeId` simc keeps as a static, which the C reads only.
#[repr(transparent)]
pub(crate) struct TypeId(pub(crate) ffi::StdTypeId);

// SAFETY: a `TypeId`'s name is a `'static` string and its drop a function, neither of which anything writes; the C
// compares it by address and calls its drop. Tested by test/t/106_cport_simc.pl:
// simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
unsafe impl Sync for TypeId {}

/// COMPRESS_DICTIONARY is the type of the `host::CompressDictionary` boxes simc hands the C.
pub(crate) static COMPRESS_DICTIONARY: TypeId = TypeId(ffi::StdTypeId {
    name: c"host::CompressDictionary".as_ptr(),
    size: size_of::<*mut c_void>(),
    align: align_of::<*mut c_void>(),
    drop: Some(compress_dictionary_drop),
});

/// DECOMPRESS_DICTIONARY is the type of the `host::DecompressDictionary` boxes simc hands the C.
pub(crate) static DECOMPRESS_DICTIONARY: TypeId = TypeId(ffi::StdTypeId {
    name: c"host::DecompressDictionary".as_ptr(),
    size: size_of::<*mut c_void>(),
    align: align_of::<*mut c_void>(),
    drop: Some(decompress_dictionary_drop),
});

/// TAKEN is the type of the `sim::Taken` a snapshot simc lends the C names as its own, which the snapshot borrows.
pub(crate) static TAKEN: TypeId = TypeId(ffi::StdTypeId {
    name: c"sim::Taken".as_ptr(),
    size: size_of::<sim::Taken>(),
    align: align_of::<sim::Taken>(),
    drop: None,
});

/// compress_dictionary_drop drops a compression dictionary simc handed the C, whose block the C then frees.
///
/// # Safety
///
/// `value` is the block of a `StdAny` of `COMPRESS_DICTIONARY` `any` made, dropped once.
unsafe extern "C" fn compress_dictionary_drop(value: *mut c_void) {
    // SAFETY: the caller's contract, a block holding the box `handed` made. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
    drop(unsafe { crate::api::taken::<host::CompressDictionary>(value.cast::<*mut c_void>().read()) });
}

/// decompress_dictionary_drop drops a decompression dictionary simc handed the C, whose block the C then frees.
///
/// # Safety
///
/// `value` is the block of a `StdAny` of `DECOMPRESS_DICTIONARY` `any` made, dropped once.
unsafe extern "C" fn decompress_dictionary_drop(value: *mut c_void) {
    // SAFETY: the caller's contract, a block holding the box `handed` made. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
    drop(unsafe { crate::api::taken::<host::DecompressDictionary>(value.cast::<*mut c_void>().read()) });
}

/// any hands the C a Rust value as a `Box<dyn Any>` of the C's: a block of the C's allocator holding the value's own
/// box, of a type whose drop takes the box back.
pub(crate) fn any<T>(value: T, type_id: &'static TypeId) -> ffi::StdAny {
    let block = allocated(type_id.0.size, type_id.0.align);
    if block.is_null() {
        std::panic::panic_any(crate::entry::OutOfMemory);
    }
    let boxed = crate::api::handed(value);
    // SAFETY: `block` is a fresh block of a pointer's layout. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
    unsafe { block.cast::<*mut c_void>().write(boxed) };
    ffi::StdAny { type_: &type_id.0, value: block }
}

/// any_ref reads back a Rust value `any` handed the C.
///
/// # Safety
///
/// `any` is a `StdAny` `any` made of `T` with `type_id`, not yet dropped, which outlives `'a`.
pub(crate) unsafe fn any_ref<'a, T>(any: &ffi::StdAny, type_id: &'static TypeId) -> &'a T {
    assert!(std::ptr::eq(any.type_, &type_id.0), "a value the C hands simc is one simc made, of its type");
    // SAFETY: the caller's contract, a block holding a box of a `T`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
    unsafe { &*any.value.cast::<*mut c_void>().read().cast::<T>() }
}

/// CCount is a C kind's count function, which reads the limits it is passed.
type CCount = unsafe extern "C" fn(*const ffi::HostSharedLimits) -> usize;

/// COUNT_SLOTS is how many count functions of C kinds simc serves, more than the crates' kinds have.
const COUNT_SLOTS: usize = 16;

/// COUNTS are the C kinds' count functions in the order simc first saw each, each one's place its slot.
static COUNTS: Mutex<Vec<CCount>> = Mutex::new(Vec::new());

/// counted is a slot's count function in the Rust's form: the slot's C count function on the limits.
fn counted<const N: usize>(limits: &Limits) -> usize {
    let count = COUNTS.lock().unwrap_or_else(PoisonError::into_inner)[N];
    let limits = ffi::HostSharedLimits {
        processes: limits.processes,
        delta_pages: limits.delta_pages,
        relation_locks: limits.relation_locks,
    };
    // SAFETY: a C kind's count function reads only the limits it is passed. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
    unsafe { count(&limits) }
}

/// count_slots makes the table of the slots' count functions.
macro_rules! count_slots {
    ($($n:literal)*) => { [$(counted::<$n> as fn(&Limits) -> usize),*] };
}

/// COUNTED are the slots' count functions in the Rust's form, slot N's at N.
static COUNTED: [fn(&Limits) -> usize; COUNT_SLOTS] = count_slots!(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15);

/// count_of returns the Rust's form of a C count function, failing with an internal error once simc serves no more.
fn count_of(count: Option<CCount>) -> Result<fn(&Limits) -> usize, Error> {
    let count = count.expect("a kind's form has its count function");
    let mut counts = COUNTS.lock().unwrap_or_else(PoisonError::into_inner);
    let slot = match counts.iter().position(|&listed| std::ptr::fn_addr_eq(listed, count)) {
        Some(slot) => slot,
        None if counts.len() < COUNT_SLOTS => {
            counts.push(count);
            counts.len() - 1
        }
        None => {
            let message = format!("simc serves at most {COUNT_SLOTS} count functions of C kinds");
            return Err(Error::fork(b"XX000", message));
        }
    };
    Ok(COUNTED[slot])
}

/// CRelease is a C owned kind's release, which frees the record at a slot.
type CRelease = unsafe extern "C" fn(u32);

/// RELEASE_SLOTS is how many releases of C owned kinds simc serves, more than the crates' owned kinds have.
const RELEASE_SLOTS: usize = 16;

/// RELEASES are the C owned kinds' releases in the order simc first saw each, each one's place its slot.
static RELEASES: Mutex<Vec<CRelease>> = Mutex::new(Vec::new());

/// Releasing is a C release's call through the C's catch: the release and the record's slot.
struct Releasing {
    /// release is the C owned kind's release.
    release: CRelease,
    /// slot is the slot of the record it frees.
    slot: u32,
}

/// release_caught calls a C release inside the C's catch.
///
/// # Safety
///
/// `releasing` is a `Releasing` `released` made for the call.
unsafe extern "C" fn release_caught(releasing: *mut c_void) {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::an_owned_record_is_released_by_its_owners_abort_unless_forgotten.
    unsafe {
        let releasing = &*releasing.cast::<Releasing>();
        (releasing.release)(releasing.slot);
    }
}

/// released is a slot's release in the Rust's form: the slot's C release on the record's slot, a C panic in it raised
/// again as the Rust's.
fn released<const N: usize>(slot: u32) {
    let release = RELEASES.lock().unwrap_or_else(PoisonError::into_inner)[N];
    let mut releasing = Releasing { release, slot };
    let mut caught = MaybeUninit::<ffi::StdCaught>::uninit();
    // SAFETY: `std_catch_unwind` calls `release_caught` with `releasing`, reports whether it returned, and writes the
    // payload of a panic it caught. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_c_releases_panic_unwinds_the_owners_abort_with_its_payload.
    let returned =
        unsafe { ffi::std_catch_unwind(Some(release_caught), (&raw mut releasing).cast(), caught.as_mut_ptr()) };
    if !returned {
        // SAFETY: the catch wrote the caught payload, which the entry raises in the C unchanged. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_c_releases_panic_unwinds_the_owners_abort_with_its_payload.
        std::panic::resume_unwind(Box::new(crate::entry::Caught(unsafe { caught.assume_init() })));
    }
}

/// release_slots makes the table of the slots' releases.
macro_rules! release_slots {
    ($($n:literal)*) => { [$(released::<$n> as fn(u32)),*] };
}

/// RELEASED are the slots' releases in the Rust's form, slot N's at N.
static RELEASED: [fn(u32); RELEASE_SLOTS] = release_slots!(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15);

/// release_of returns the Rust's form of a C release, panicking once simc serves no more, since the door that takes it
/// can't fail.
pub(crate) fn release_of(release: Option<CRelease>) -> fn(u32) {
    let release = release.expect("an owned record comes with its kind's release");
    let mut releases = RELEASES.lock().unwrap_or_else(PoisonError::into_inner);
    let slot = match releases.iter().position(|&listed| std::ptr::fn_addr_eq(listed, release)) {
        Some(slot) => slot,
        None => {
            assert!(releases.len() < RELEASE_SLOTS, "simc serves at most {RELEASE_SLOTS} releases of C owned kinds");
            releases.push(release);
            releases.len() - 1
        }
    };
    RELEASED[slot]
}

/// ARRAYS are the C kinds' arrays in the Rust's form, by kind id, each made once and never changed or removed, so a
/// kind lends them for the process's life.
static ARRAYS: Mutex<Vec<KindArrays>> = Mutex::new(Vec::new());

/// KindArrays is a C kind's arrays in the Rust's form: the kind's id, and each array's count of entries and entry size.
type KindArrays = (u64, Vec<(usize, usize)>);

/// kind reads a C kind as the Rust's, its count functions calling the C's through their slots.
///
/// # Safety
///
/// `kind` is a kind the C's doors pass, whose blobs, arrays, and names are its shape's constants, which live for the
/// process, and whose form's tag names its union's field.
pub(crate) unsafe fn kind(kind: &ffi::HostSharedKind) -> Result<Kind, Error> {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
    let blobs = unsafe { slice(kind.blobs, kind.blobs_len) };
    let arrays = {
        let mut arrays = ARRAYS.lock().unwrap_or_else(PoisonError::into_inner);
        let at = match arrays.iter().position(|(id, _)| *id == kind.id) {
            Some(at) => at,
            None => {
                // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
                // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
                let read = unsafe { slice(kind.arrays, kind.arrays_len) };
                arrays.push((kind.id, read.iter().map(|array| (array.f0, array.f1)).collect()));
                arrays.len() - 1
            }
        };
        let made = &arrays[at].1;
        // SAFETY: an entry of `ARRAYS` is never changed or removed, so its vector's block stays where it is for the
        // process's life, whatever the outer vector's growth moves. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
        unsafe { slice(made.as_ptr(), made.len()) }
    };
    // SAFETY: the caller's contract, the tag naming the union's field. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
    let form = unsafe {
        match kind.form.tag {
            ffi::HostSharedFormTag_HOST_SHARED_FORM_POOL => Form::Pool(count_of(kind.form.u.pool)?),
            ffi::HostSharedFormTag_HOST_SHARED_FORM_SINGLE => Form::Single { run: count_of(kind.form.u.single.run)? },
            ffi::HostSharedFormTag_HOST_SHARED_FORM_AGED => Form::Aged {
                count: count_of(kind.form.u.aged.count)?,
                names: slice(kind.form.u.aged.names, kind.form.u.aged.names_len),
            },
            _ => Form::Process { kept: kind.form.u.process.kept },
        }
    };
    Ok(Kind { id: kind.id, locks: kind.locks, flags: kind.flags, atomics: kind.atomics, blobs, arrays, form })
}
