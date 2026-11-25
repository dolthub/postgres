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

//! The simulation's node caches (design item 14's S5) and resource owners: the shared cache and each process's own,
//! in process memory, with the server's clocks of fixed frames, claimed victims, and per-process pins, and each
//! process's resource owners nested like savepoints, whose abort releases what each remembered.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use host::cache::{self, Bytes, Found, KEY, Key, Placement};

use crate::shared::proc;

/// MAX_USAGE is the usage count a pin raises a frame to at most, Postgres' `BM_MAX_USAGE_COUNT`.
const MAX_USAGE: u8 = 5;

/// STALE is the byte a frame holds before its fill, as a server's frame holds its last node, so a fill that leaves a
/// byte unwritten shows.
const STALE: u8 = 0xa5;

/// FRAMES_PER_CLASS is how many frames of each class the shared cache holds unless a test sizes it.
const FRAMES_PER_CLASS: usize = 16;

/// Frame is a frame of a cache: the key it holds, its pins by process, its usage count, whether its bytes are whole,
/// and its bytes, which a new fill replaces while the bytes lent before stay alive.
#[derive(Default)]
struct Frame {
    key: Option<[u8; KEY]>,
    pins: BTreeMap<u32, u32>,
    usage: u8,
    valid: bool,
    bytes: Option<Arc<[u8]>>,
}

impl Frame {
    /// pinned reports whether any process pins the frame.
    fn pinned(&self) -> bool {
        !self.pins.is_empty()
    }

    /// pin adds a process's pin.
    fn pin(&mut self, proc: u32) {
        *self.pins.entry(proc).or_default() += 1;
        self.usage = (self.usage + 1).min(MAX_USAGE);
    }

    /// unpin removes a process's pin, failing the test of a process that unpins a frame it doesn't pin, which in the
    /// server would take another process's pin from the frame's one count.
    fn unpin(&mut self, proc: u32) {
        let count = self.pins.get_mut(&proc).expect("a process unpins only a frame it pins");
        *count -= 1;
        if *count == 0 {
            self.pins.remove(&proc);
        }
    }
}

/// Cache is a node cache: its frames, each class's range of them and clock hand, and its table of keys.
struct Cache {
    frames: Vec<Frame>,
    classes: Vec<(usize, usize, usize)>,
    table: BTreeMap<[u8; KEY], u32>,
}

impl Cache {
    /// new returns an empty cache of a number of frames in each class.
    fn new(counts: &[usize]) -> Cache {
        let mut frames = Vec::new();
        let mut classes = Vec::new();
        for &count in counts {
            classes.push((frames.len(), count, 0));
            frames.extend((0..count).map(|_| Frame::default()));
        }
        Cache { frames, classes, table: BTreeMap::new() }
    }

    /// claim sweeps a class's clock for a frame no process pins or used lately, lowering usage counts as it passes,
    /// and pins it for a process, giving up after two passes.
    fn claim(&mut self, class: usize, me: u32) -> Option<(u32, Option<[u8; KEY]>)> {
        let (start, count, _) = self.classes[class];
        for _ in 0..2 * count {
            let at = start + self.classes[class].2;
            self.classes[class].2 = (self.classes[class].2 + 1) % count;
            let frame = &mut self.frames[at];
            if frame.pinned() {
                continue;
            }
            if frame.usage > 0 {
                frame.usage -= 1;
                continue;
            }
            frame.pins.insert(me, 1);
            return Some((at as u32, frame.key));
        }
        None
    }

    /// found returns a frame's bytes, for a process that pinned it.
    fn found(&self, frame: u32) -> Found {
        let bytes = self.frames[frame as usize].bytes.clone().expect("a valid frame holds bytes");
        Found { frame, bytes: Bytes::Lent(bytes) }
    }
}

/// Caches is the simulation's node caches: the shared one and each process's own.
struct Caches {
    shared: Option<Cache>,
    local: BTreeMap<u32, Cache>,
}

/// CACHES is the simulation's node caches.
static CACHES: Mutex<Caches> = Mutex::new(Caches { shared: None, local: BTreeMap::new() });

/// caches returns the simulation's node caches, the shared one made at its default size the first time.
fn caches() -> MutexGuard<'static, Caches> {
    let mut caches = CACHES.lock().unwrap_or_else(PoisonError::into_inner);
    if caches.shared.is_none() {
        caches.shared = Some(Cache::new(&[FRAMES_PER_CLASS; cache::CLASSES.len()]));
    }
    caches
}

/// cache returns a placement's cache for a process, or None for a process that made no cache of its own.
fn cache(caches: &mut Caches, placement: Placement, me: u32) -> Option<&mut Cache> {
    match placement {
        Placement::Server => caches.shared.as_mut(),
        Placement::Backend => caches.local.get_mut(&me),
    }
}

/// size_shared empties the shared cache and gives it a number of frames in each class, for a test of eviction.
pub fn size_shared(counts: &[usize; cache::CLASSES.len()]) {
    caches().shared = Some(Cache::new(counts));
}

/// pins returns how many pins every process holds on the shared frame that holds a key, or None when none does.
pub fn pins(key: &Key) -> Option<u32> {
    let mut caches = caches();
    let shared = caches.shared.as_mut().expect("the shared cache is made");
    let frame = *shared.table.get(key.bytes())?;
    Some(shared.frames[frame as usize].pins.values().sum())
}

/// REFUSED is whether the next `contains` is refused, which a test of where a cache door's error goes sets.
static REFUSED: AtomicBool = AtomicBool::new(false);

/// refuse_next_contains makes the next `contains` fail as the server's does when its lock can't be taken.
pub fn refuse_next_contains() {
    REFUSED.store(true, Ordering::Relaxed);
}

/// refused reports whether a test refused this `contains`, once.
pub(crate) fn refused() -> bool {
    REFUSED.swap(false, Ordering::Relaxed)
}

/// contains reports whether a cache's table names a key, changing nothing.
pub(crate) fn contains(placement: Placement, key: &[u8; KEY]) -> bool {
    cache(&mut caches(), placement, proc()).is_some_and(|made| made.table.contains_key(key))
}

/// get pins the frame that holds a key's node whole.
pub(crate) fn get(placement: Placement, key: &[u8; KEY]) -> Option<Found> {
    let me = proc();
    let mut caches = caches();
    let cache = cache(&mut caches, placement, me)?;
    let frame = *cache.table.get(key)?;
    if !cache.frames[frame as usize].valid {
        return None;
    }
    cache.frames[frame as usize].pin(me);
    Some(cache.found(frame))
}

/// Claimed is a victim an install claimed for its process, which unpins it when it drops unless the install kept it,
/// as the server's guard does on every way out of an install. While it lives the process's termination waits, since
/// the server checks for none between a claim and the install's end.
struct Claimed {
    placement: Placement,
    frame: u32,
    kept: bool,
}

impl Claimed {
    /// new guards a frame the running process just claimed.
    fn new(placement: Placement, frame: u32) -> Claimed {
        crate::run::hold(true);
        Claimed { placement, frame, kept: false }
    }
}

impl Drop for Claimed {
    fn drop(&mut self) {
        if !self.kept {
            unpin(self.placement, self.frame);
        }
        crate::run::hold(false);
    }
}

/// install claims a victim, rechecks it after the yield where the server takes its partition locks, and has `fill`
/// write its `len` bytes, or pins the frame another process installed the key in meanwhile, or returns None when no
/// frame is free. A victim the table already names by the key, as a fill that never finished leaves one, is filled
/// where it is.
pub(crate) fn install(
    placement: Placement,
    key: &[u8; KEY],
    len: usize,
    fill: &mut dyn FnMut(&mut [u8]),
) -> Option<Found> {
    let me = proc();
    let class = cache::class(len)?;
    let count = cache(&mut caches(), placement, me)?.classes[class].1;
    for _ in 0..2 * count {
        let (victim, old) = cache(&mut caches(), placement, me)?.claim(class, me)?;
        let mut claimed = Claimed::new(placement, victim);
        crate::run::yield_point();
        let mut all = caches();
        let made = cache(&mut all, placement, me).expect("a cache is never unmade while its process lives");
        let frame = &made.frames[victim as usize];
        if frame.pins.len() != 1 || frame.pins.get(&me) != Some(&1) || frame.key != old {
            continue;
        }
        let named = made.table.get(key).copied();
        if let Some(other) = named.filter(|&other| other != victim) {
            if !made.frames[other as usize].valid {
                return None;
            }
            made.frames[other as usize].pin(me);
            return Some(made.found(other));
        }
        if named.is_none() {
            if let Some(old) = old {
                made.table.remove(&old);
            }
            made.table.insert(*key, victim);
        }
        let frame = &mut made.frames[victim as usize];
        frame.key = Some(*key);
        frame.valid = false;
        frame.usage = 1;
        drop(all);
        crate::run::yield_point();
        let mut bytes = vec![STALE; len];
        fill(&mut bytes);
        let mut all = caches();
        let made = cache(&mut all, placement, me).expect("a cache is never unmade while its process lives");
        let frame = &mut made.frames[victim as usize];
        frame.bytes = Some(Arc::from(bytes));
        frame.valid = true;
        claimed.kept = true;
        return Some(made.found(victim));
    }
    None
}

/// unpin gives up a process's pin of a frame.
pub(crate) fn unpin(placement: Placement, frame: u32) {
    let me = proc();
    let mut caches = CACHES.lock().unwrap_or_else(PoisonError::into_inner);
    if let Some(cache) = cache(&mut caches, placement, me) {
        cache.frames[frame as usize].unpin(me);
    }
}

/// make_local makes a process's own cache of a size, once.
pub(crate) fn make_local(bytes: usize) {
    let me = proc();
    caches().local.entry(me).or_insert_with(|| Cache::new(&cache::frames(bytes)));
}

/// Owners is a process's resource owners, innermost last, each with what it remembers.
type Owners = Vec<(u64, Vec<Remembered>)>;

/// Remembered is what a resource owner remembers: a node cache pin by its datum, or an owned record by its datum with
/// its kind's release.
#[derive(Clone, Copy)]
enum Remembered {
    Pin(u64),
    Owned(u64, fn(u32)),
}

impl Remembered {
    /// datum returns the datum the resource owner was given.
    fn datum(self) -> u64 {
        match self {
            Remembered::Pin(datum) | Remembered::Owned(datum, _) => datum,
        }
    }
}

/// OWNERS is each process's resource owners.
static OWNERS: Mutex<BTreeMap<u32, Owners>> = Mutex::new(BTreeMap::new());

/// owners returns each process's resource owners.
fn owners() -> MutexGuard<'static, BTreeMap<u32, Owners>> {
    OWNERS.lock().unwrap_or_else(PoisonError::into_inner)
}

/// stack returns a process's resource owners, its transaction's outermost one made the first time.
fn stack(owners: &mut BTreeMap<u32, Owners>, me: u32) -> &mut Owners {
    owners.entry(me).or_insert_with(|| vec![(1, Vec::new())])
}

/// owner_current names the running process's innermost resource owner.
pub(crate) fn owner_current() -> u64 {
    stack(&mut owners(), proc()).last().expect("a process has its outermost owner").0
}

/// owner_top names the running process's outermost resource owner, its top-level transaction's.
pub(crate) fn owner_top() -> u64 {
    stack(&mut owners(), proc())[0].0
}

/// owner_remember has a resource owner of the running process remember a pin.
pub(crate) fn owner_remember(owner: u64, pin: u64) {
    remember(owner, Remembered::Pin(pin));
}

/// owner_forget has a resource owner of the running process forget a pin.
pub(crate) fn owner_forget(owner: u64, pin: u64) {
    forget(owner, pin, true);
}

/// owned_remember has a resource owner of the running process remember an owned record with its kind's release.
pub(crate) fn owned_remember(owner: u64, record: u64, release: fn(u32)) {
    remember(owner, Remembered::Owned(record, release));
}

/// owned_forget has a resource owner of the running process forget an owned record.
pub(crate) fn owned_forget(owner: u64, record: u64) {
    forget(owner, record, false);
}

/// remember has a resource owner of the running process remember a thing.
fn remember(owner: u64, thing: Remembered) {
    if let Some((_, things)) = stack(&mut owners(), proc()).iter_mut().find(|(id, _)| *id == owner) {
        things.push(thing);
    }
}

/// forget has a resource owner of the running process forget a pin or an owned record by its datum.
fn forget(owner: u64, datum: u64, pin: bool) {
    let mut owners = owners();
    let Some(stack) = owners.get_mut(&proc()) else { return };
    if let Some((_, things)) = stack.iter_mut().find(|(id, _)| *id == owner) {
        things.retain(|&remembered| (matches!(remembered, Remembered::Pin(_)), remembered.datum()) != (pin, datum));
    }
}

/// begin_owner starts a subtransaction's resource owner inside the running process's innermost one, as a savepoint
/// does.
pub fn begin_owner() {
    let mut owners = owners();
    let stack = stack(&mut owners, proc());
    let id = stack.last().expect("a process has its outermost owner").0 + 1;
    stack.push((id, Vec::new()));
}

/// abort_owner ends the innermost subtransaction's resource owner and releases what it remembered, as a rollback to
/// a savepoint does, leaving what the owners around it remember: its pins first, then its owned records, in
/// Postgres' order of their kinds.
pub fn abort_owner() {
    let things = {
        let mut owners = owners();
        let stack = stack(&mut owners, proc());
        match stack.len() > 1 {
            true => stack.pop().expect("an inner owner is there").1,
            false => std::mem::take(&mut stack[0].1),
        }
    };
    for thing in &things {
        if let Remembered::Pin(pin) = *thing {
            cache::released(pin);
        }
    }
    for thing in things {
        if let Remembered::Owned(record, release) = thing {
            release(host::owned::remembered(record).1);
        }
    }
}

/// left takes every process's resource owners away and returns the kinds of the owned records they still remember,
/// which a test's end finds none of, since each record's transaction ends it.
pub(crate) fn left() -> Vec<u8> {
    let owners = std::mem::take(&mut *owners());
    let things = owners.into_values().flatten().flat_map(|(_, things)| things);
    things
        .filter_map(|thing| match thing {
            Remembered::Owned(record, _) => Some(host::owned::remembered(record).0),
            Remembered::Pin(_) => None,
        })
        .collect()
}

/// exit gives up what a process's exit leaves to Postgres: its own cache and its resource owners. Its pins of
/// shared frames are its exit's to release, through host's `release_all`.
pub(crate) fn exit(me: u32) {
    CACHES.lock().unwrap_or_else(PoisonError::into_inner).local.remove(&me);
    owners().remove(&me);
}

/// discard drops what a failed run's stuck process left, whose exit never ran: its pins of shared frames, then what
/// its exit would give up.
pub(crate) fn discard(me: u32) {
    if let Some(shared) = CACHES.lock().unwrap_or_else(PoisonError::into_inner).shared.as_mut() {
        for frame in &mut shared.frames {
            frame.pins.remove(&me);
        }
    }
    exit(me);
}

/// reset discards every cache and resource owner, as a crash's restart starts shared memory afresh.
pub(crate) fn reset() {
    let mut caches = CACHES.lock().unwrap_or_else(PoisonError::into_inner);
    caches.shared = None;
    caches.local.clear();
    owners().clear();
}

#[cfg(test)]
mod tests {
    use std::panic::{AssertUnwindSafe, catch_unwind};
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};

    use host::cache::{self, Key, Placement, Read};
    use host::io::Io;

    use super::{abort_owner, begin_owner, pins, size_shared};
    use crate::{Ending, Outcome, Report, Run, as_actor};

    /// key returns the key of the node numbered `n`: one location's, with the number at the end of its address.
    fn key(n: u8) -> Key {
        let mut address = [0; cache::ADDRESS];
        address[cache::ADDRESS - 1] = n;
        cache::key(1, &address, &[0; cache::LOCATOR])
    }

    /// install installs a node of `len` bytes of `n` under key `n` in the shared cache.
    fn install<'io>(io: &mut Io, n: u8, len: usize) -> Read<'io> {
        cache::install(io, Placement::Server, &key(n), len, |bytes| bytes.fill(n)).unwrap()
    }

    /// cached reports whether a read is pinned in its cache.
    fn cached(read: &Read<'_>) -> bool {
        matches!(read, Read::Cached(_))
    }

    /// ended_ok asserts that every actor of a run returned without an error.
    fn ended_ok(report: &Report) {
        let Outcome::Ended(endings) = &report.outcome else { panic!("the run didn't end: {:?}", report.outcome) };
        for (name, ending) in endings {
            assert_eq!(*ending, Ending::Returned(Ok(())), "{name} failed");
        }
    }

    /// failed returns a test actor's error.
    fn failed(message: &str) -> host::Error {
        host::Error::fork(b"XX000", message.to_owned())
    }

    /// A node one backend installs is a hit for another, which pins the same frame and reads its bytes.
    #[test]
    fn a_hit_after_another_backends_install_pins_its_frame() {
        let _process = crate::process();
        crate::install();
        size_shared(&[2, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        let first = as_actor(1, || install(io, 1, 100));
        assert!(cached(&first));
        let second = as_actor(2, || cache::get(io, Placement::Server, &key(1)).unwrap()).expect("a hit");
        assert_eq!(&*second, &[1; 100][..]);
        assert_eq!(pins(&key(1)), Some(2));
        as_actor(1, || drop(first));
        as_actor(2, || drop(second));
        assert_eq!(pins(&key(1)), Some(0));
    }

    /// The clock evicts only unpinned frames, and with every frame of a size pinned an install gives the bytes back
    /// without waiting.
    #[test]
    fn the_clock_evicts_only_unpinned_frames_and_a_full_class_reads_uncached() {
        let _process = crate::process();
        crate::install();
        size_shared(&[2, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        as_actor(1, || {
            let one = install(io, 1, 100);
            let two = install(io, 2, 100);
            assert!(cached(&one) && cached(&two));
            let uncached = install(io, 3, 100);
            assert!(!cached(&uncached), "an install took a pinned frame");
            assert_eq!(&*uncached, &[3; 100][..], "an uncached read wasn't filled");
            drop(uncached);
            drop(two);
            assert!(cached(&install(io, 3, 100)), "an install found no unpinned frame");
            assert!(cache::get(io, Placement::Server, &key(1)).unwrap().is_some(), "a pinned node was evicted");
            assert!(cache::get(io, Placement::Server, &key(2)).unwrap().is_none(), "the unpinned node stayed");
        });
    }

    /// Two backends whose installs of one key interleave end with one frame holding it, which both pin.
    #[test]
    fn two_backends_installing_one_key_end_with_one_frame() {
        crate::install();
        let mut run = Run::new(3);
        size_shared(&[4, 0, 0, 0, 0]);
        let reads: Arc<std::sync::Mutex<Vec<Vec<u8>>>> = Arc::default();
        for (name, n) in [("a", 1u8), ("b", 2u8)] {
            let reads = reads.clone();
            run.actor(name, move |io| {
                let read = cache::install(io, Placement::Server, &key(9), 100, |bytes| bytes.fill(n))?;
                if !cached(&read) {
                    return Err(failed("an install of a key found no frame"));
                }
                reads.lock().unwrap().push(read.to_vec());
                while reads.lock().unwrap().len() < 2 {
                    host::unique();
                }
                Ok(())
            });
        }
        run.script(&[("a", 3), ("b", 6), ("a", u64::MAX)]);
        ended_ok(&run.finish());
        let reads = reads.lock().unwrap();
        assert_eq!(reads[0], reads[1], "two installs of one key read different frames");
    }

    /// A victim one backend claimed is never another's: with one frame, the second install reads uncached.
    #[test]
    fn a_claimed_victim_is_claimed_once() {
        crate::install();
        let mut run = Run::new(5);
        size_shared(&[1, 0, 0, 0, 0]);
        let done: Arc<AtomicBool> = Arc::default();
        let finished = done.clone();
        run.actor("a", move |io| {
            let read = install(io, 1, 100);
            finished.store(true, Ordering::Relaxed);
            match cached(&read) && *read == [1; 100][..] {
                true => Ok(()),
                false => Err(failed("the claimant lost its frame")),
            }
        });
        run.actor("b", move |io| {
            let read = install(io, 2, 100);
            match cached(&read) {
                true => Err(failed("a second install took a claimed victim")),
                false => Ok(()),
            }
        });
        run.script(&[("a", 3), ("b", u64::MAX), ("a", u64::MAX)]);
        ended_ok(&run.finish());
        assert!(done.load(Ordering::Relaxed));
    }

    /// A reader that pins the old key between an installer's claim and its recheck keeps its bytes, and the installer
    /// takes another victim or reads uncached.
    #[test]
    fn a_reader_pinning_the_old_key_after_a_claim_keeps_its_bytes() {
        crate::install();
        let mut run = Run::new(7);
        size_shared(&[1, 0, 0, 0, 0]);
        as_actor(1, || drop(install(&mut Io::testing(), 1, 100)));
        let flags: [Arc<AtomicBool>; 2] = Default::default();
        let [pinned, installed] = flags.clone();
        run.actor("installer", move |io| {
            let read = install(io, 2, 100);
            installed.store(true, Ordering::Relaxed);
            match cached(&read) {
                true => Err(failed("an install took a frame a reader pinned")),
                false => Ok(()),
            }
        });
        let [pinning, installed] = flags;
        run.actor("reader", move |io| {
            let read = cache::get(io, Placement::Server, &key(1))?.ok_or_else(|| failed("the old key was gone"))?;
            pinning.store(true, Ordering::Relaxed);
            while !installed.load(Ordering::Relaxed) {
                host::unique();
            }
            match *read == [1; 100][..] {
                true => Ok(()),
                false => Err(failed("a reader's pinned bytes changed under it")),
            }
        });
        run.script(&[("installer", 3), ("reader", 3), ("installer", u64::MAX), ("reader", u64::MAX)]);
        ended_ok(&run.finish());
        assert!(pinned.load(Ordering::Relaxed));
    }

    /// A pin taken outside a savepoint survives a rollback to it, a rollback releases the pins taken since, and a pin
    /// the rollback released unpins nothing more when it drops, not even a later pin that took its slot in the pin
    /// table.
    #[test]
    fn a_rollback_releases_only_its_own_pins_and_their_drop_unpins_nothing_more() {
        let _process = crate::process();
        crate::install();
        size_shared(&[4, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        let other = as_actor(2, || {
            drop(install(io, 2, 100));
            cache::get(io, Placement::Server, &key(2)).unwrap().expect("a hit")
        });
        as_actor(1, || {
            let outer = install(io, 1, 100);
            begin_owner();
            let inner = cache::get(io, Placement::Server, &key(2)).unwrap().expect("a hit");
            assert_eq!(pins(&key(2)), Some(2));
            abort_owner();
            assert_eq!(pins(&key(2)), Some(1), "a rollback kept its pin");
            assert_eq!(pins(&key(1)), Some(1), "a rollback released a pin taken before its savepoint");
            let later = cache::get(io, Placement::Server, &key(2)).unwrap().expect("a hit");
            assert_eq!(pins(&key(2)), Some(2));
            drop(inner);
            assert_eq!(pins(&key(2)), Some(2), "a released pin's drop unpinned the later pin in its slot");
            drop(later);
            assert_eq!(pins(&key(2)), Some(1), "a released pin's drop unpinned another backend's pin");
            drop(outer);
        });
        as_actor(2, || drop(other));
        assert_eq!(pins(&key(2)), Some(0));
    }

    /// A kept pin resumes with its node's bytes while its resource owner lasts, across a rollback to a savepoint
    /// taken after it, and gives nothing once its owner's rollback released it.
    #[test]
    fn a_kept_pin_resumes_until_its_owner_releases_it() {
        let _process = crate::process();
        crate::install();
        size_shared(&[4, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        as_actor(1, || {
            let Read::Cached(outer) = install(io, 1, 100) else { panic!("an install found no frame") };
            let outer = outer.keep();
            begin_owner();
            let Read::Cached(inner) = install(io, 2, 100) else { panic!("an install found no frame") };
            let inner = inner.keep();
            let resumed = outer.resume(io).expect("a kept pin didn't resume while its owner lasts");
            assert_eq!(&*resumed, &[1; 100][..]);
            let outer = resumed.keep();
            abort_owner();
            assert!(inner.resume(io).is_none(), "a pin its owner's rollback released resumed");
            assert_eq!(pins(&key(2)), Some(0), "a released pin stayed");
            let resumed = outer.resume(io).expect("a rollback released a pin kept before its savepoint");
            assert_eq!(&*resumed, &[1; 100][..]);
            drop(resumed);
            assert_eq!(pins(&key(1)), Some(0));
        });
    }

    /// A fill that panics leaves its frame named by its key with no pin, a miss for a reader, and the key's next
    /// install, which the clock hands that frame, fills it.
    #[test]
    fn a_fill_that_panics_leaves_its_frame_unpinned_for_its_keys_next_install() {
        let _process = crate::process();
        crate::install();
        size_shared(&[1, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        as_actor(1, || {
            let unwound = catch_unwind(AssertUnwindSafe(|| {
                cache::install(io, Placement::Server, &key(1), 100, |_| panic!("a fill's bug")).map(drop)
            }));
            assert!(unwound.is_err(), "a fill's panic didn't unwind");
            assert_eq!(pins(&key(1)), Some(0), "a fill that unwound left its frame pinned");
            assert!(cache::get(io, Placement::Server, &key(1)).unwrap().is_none(), "an unfilled frame was read");
            let again = install(io, 1, 100);
            assert!(cached(&again), "the key's next install didn't fill the frame its failed fill left");
            assert_eq!(&*again, &[1; 100][..]);
        });
    }

    /// A cache says it holds a key after its install and not after its frame is taken, and a frame only asked about
    /// is taken by the clock as one never read is.
    #[test]
    fn asking_for_a_key_neither_pins_nor_uses_its_frame() {
        let _process = crate::process();
        crate::install();
        size_shared(&[1, 0, 0, 0, 0]);
        let io = &mut Io::testing();
        as_actor(1, || {
            assert!(!cache::contains(io, Placement::Server, &key(1)).unwrap());
            drop(install(io, 1, 100));
            for _ in 0..10 {
                assert!(cache::contains(io, Placement::Server, &key(1)).unwrap(), "an installed key wasn't there");
            }
            assert_eq!(pins(&key(1)), Some(0), "asking pinned a frame");
            assert!(cached(&install(io, 2, 100)), "a frame only asked about wasn't taken as one never read");
            assert!(!cache::contains(io, Placement::Server, &key(1)).unwrap(), "a taken frame's key was still there");
            assert!(cache::contains(io, Placement::Server, &key(2)).unwrap());
        });
    }

    /// A terminated backend's pins are released at its exit.
    #[test]
    fn a_terminated_backends_pins_go_at_its_exit() {
        crate::install();
        let mut run = Run::new(9);
        size_shared(&[2, 0, 0, 0, 0]);
        run.actor("held", |io| {
            let _read = install(io, 1, 100);
            loop {
                host::unique();
            }
        });
        run.terminate_at("held", 50);
        let report = run.finish();
        assert!(matches!(&report.outcome, Outcome::Ended(endings) if endings[0].1 == Ending::Terminated));
        assert_eq!(pins(&key(1)), Some(0), "a terminated backend's pin stayed");
    }
}
