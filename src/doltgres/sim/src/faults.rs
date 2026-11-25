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

//! The storage simulation's file faults: every seam of the simulation's host that changes a file under a simulated
//! directory first tells `before`, which keeps what a power loss would take back and may fail the seam with a crash or
//! a full disk the seed chose, and `Sim::recover` rolls the directory back to what was durable.
//!
//! A rename within a directory is one change, durable at the directory's sync. A file renamed into another directory is
//! two, the source's removal and the target's addition, each durable at its own directory's sync, so that a crash can
//! leave it under both names or neither, which recovery must survive anyway. A directory renamed into another is one
//! change, durable once both parents are synced, since every filesystem a data directory runs on (NTFS, APFS, ext4, XFS,
//! btrfs) commits a rename in one transaction (STORAGE_DESIGN's filesystem assumption).
//!
//! The host's clock, process id, and unique numbers come from the simulation the thread started, a clock that a
//! reading moves a microsecond, so that the times and names the storage crates write depend on the seed alone.
#![expect(
    clippy::disallowed_methods,
    clippy::disallowed_types,
    reason = "the simulation's own state and the real files and clock it stands in front of, for tests alone"
)]

use std::collections::{BTreeMap, BTreeSet};
use std::hash::{DefaultHasher, Hasher};
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::ThreadId;
use std::time::{Duration, Instant, SystemTime};

/// locked takes one of the simulation's own locks, whose state a panicking test leaves as it was.
fn locked<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// SIMS are the simulations running, each over its own directory.
static SIMS: Mutex<Vec<Arc<Sim>>> = Mutex::new(Vec::new());

/// EPOCH is the time of day a simulation's clock starts at, 2026-01-01 UTC.
const EPOCH: Duration = Duration::from_secs(1_767_225_600);

/// CURRENT are the simulations threads started, by thread, whose clock and numbers the host reads.
static CURRENT: Mutex<Vec<(ThreadId, Arc<Sim>)>> = Mutex::new(Vec::new());

/// Change is what a door is about to do to the files.
pub(crate) enum Change<'a> {
    /// Bytes written at an offset of a file.
    Write(&'a Path, u64, usize),
    /// A file cut or grown to a length.
    Truncate(&'a Path),
    /// A file created, or emptied if it exists.
    Create(&'a Path),
    /// A file removed.
    Remove(&'a Path),
    /// A file or directory renamed over another.
    Rename(&'a Path, &'a Path),
    /// A directory created.
    CreateDir(&'a Path),
    /// A file's bytes made durable.
    Sync(&'a Path),
    /// A directory's entries made durable.
    SyncDir(&'a Path),
}

/// Undo is how a recovery takes back a change to a directory's entries that no sync of the directory made durable.
enum Undo {
    /// A file or directory that didn't exist, to remove.
    Created(PathBuf),
    /// A file removed, with its durable bytes, to put back.
    Removed(PathBuf, Vec<u8>),
    /// A rename within a directory to take back, with the durable bytes of the file it replaced.
    Renamed(PathBuf, PathBuf, Option<Vec<u8>>),
    /// A file a rename from another directory added, to remove, with the durable bytes of the file it replaced, to put
    /// back.
    Added(PathBuf, Option<Vec<u8>>),
    /// A directory renamed into another directory, to rename back as one step, with the parent directories not synced
    /// since, which must all be before it is durable.
    MovedDir(PathBuf, PathBuf, Vec<PathBuf>),
}

impl Undo {
    /// paths returns every path the change names, which a durable rename of a directory they lie under moves, since no
    /// recovery moves the directory back first.
    fn paths(&mut self) -> Vec<&mut PathBuf> {
        match self {
            Undo::Created(path) | Undo::Removed(path, _) | Undo::Added(path, _) => vec![path],
            Undo::Renamed(from, to, _) => vec![from, to],
            Undo::MovedDir(from, to, parents) => [from, to].into_iter().chain(parents.iter_mut()).collect(),
        }
    }
}

/// State is a simulation's record: how many door calls it saw, the faults it plans, whether it crashed, the durable
/// bytes of each file changed since its last sync, the entry changes since their directory's last sync, the last
/// write, which a crash may leave half done, its clock's readings, the unique numbers it gave, and its trace.
#[derive(Default)]
struct State {
    calls: u64,
    crash_at: Option<u64>,
    full_at: Vec<u64>,
    short_at: Vec<u64>,
    deferred_at: Vec<u64>,
    /// The files a failed sync dropped writes of, whose durable bytes no later sync moves past that sync's.
    dropped: BTreeSet<PathBuf>,
    crashed: bool,
    durable: BTreeMap<PathBuf, Option<Vec<u8>>>,
    undo: Vec<Undo>,
    last_write: Option<(PathBuf, u64, Vec<u8>)>,
    random: u64,
    ticks: u64,
    ids: u64,
    trace: Vec<String>,
}

/// Sim is a simulation of the files under a directory, which a seed drives.
pub struct Sim {
    root: PathBuf,
    state: Mutex<State>,
    /// The real monotonic clock's reading at the start, which the simulated readings count from.
    started: Instant,
}

impl Sim {
    /// start begins simulating the files under a directory and the clock of the thread that calls it, seeded so that a
    /// run repeats exactly.
    pub fn start(root: &Path, seed: u64) -> Arc<Sim> {
        let state = State { random: seed | 1, ..State::default() };
        let sim = Arc::new(Sim { root: root.to_path_buf(), state: Mutex::new(state), started: Instant::now() });
        locked(&SIMS).push(sim.clone());
        let thread = std::thread::current().id();
        let mut current = locked(&CURRENT);
        current.retain(|(started, _)| *started != thread);
        current.push((thread, sim.clone()));
        drop(current);
        sim
    }

    /// stop ends the simulation, leaving the files as they are.
    pub fn stop(self: &Arc<Self>) {
        locked(&SIMS).retain(|sim| !Arc::ptr_eq(sim, self));
        if current().is_some_and(|sim| Arc::ptr_eq(&sim, self)) {
            let thread = std::thread::current().id();
            locked(&CURRENT).retain(|(started, _)| *started != thread);
        }
    }

    /// trace returns every door call the simulation saw, in order, with its paths under the simulated directory and a
    /// digest of each write's bytes, which two runs of one seed share line for line.
    pub fn trace(&self) -> Vec<String> {
        locked(&self.state).trace.clone()
    }

    /// calls returns how many door calls that change a file the simulation has seen.
    pub fn calls(&self) -> u64 {
        locked(&self.state).calls
    }

    /// crash_at plans a crash at a door call, counting from the first, which fails it and every call after until
    /// `recover`.
    pub fn crash_at(&self, call: u64) {
        locked(&self.state).crash_at = Some(call);
    }

    /// full_at plans a full disk at a door call, which fails that call alone.
    pub fn full_at(&self, call: u64) {
        locked(&self.state).full_at.push(call);
    }

    /// short_at plans a write at a door call that a disk filling up cuts short: it writes a seeded part of its bytes and
    /// then fails, as a write the disk ran out of room in partway does.
    pub fn short_at(&self, call: u64) {
        locked(&self.state).short_at.push(call);
    }

    /// deferred_at plans a sync at a door call that fails for want of room the writes before it were never given, as a
    /// file system that allocates at sync does, after which the file's writes since its last good sync stay as a power
    /// loss would take them back, whatever later syncs report, as a kernel that drops the pages of a failed sync does.
    pub fn deferred_at(&self, call: u64) {
        locked(&self.state).deferred_at.push(call);
    }

    /// crashed reports whether the simulation crashed.
    pub fn crashed(&self) -> bool {
        locked(&self.state).crashed
    }

    /// recover takes the directory back to what a power loss at the crash leaves: every file's bytes back to its last
    /// sync, where its name is now, then every entry change no sync of its directory made durable undone, newest first,
    /// so that a rename taken back carries the synced bytes to the old name, keeping of the last write a seeded part when
    /// `torn`, as a write a crash cut short leaves. It ends the crash.
    pub fn recover(&self, torn: bool) -> std::io::Result<()> {
        let mut state = locked(&self.state);
        let torn = match (torn, state.last_write.take()) {
            (true, Some((path, offset, bytes))) => {
                let kept = (next(&mut state.random) % (bytes.len() as u64 + 1)) as usize;
                Some((path, offset, bytes[..kept].to_vec()))
            }
            _ => None,
        };
        for (path, bytes) in std::mem::take(&mut state.durable) {
            if path.exists() {
                std::fs::write(&path, bytes.unwrap_or_default())?;
            }
        }
        for undo in std::mem::take(&mut state.undo).into_iter().rev() {
            match undo {
                Undo::Created(path) if path.is_dir() => std::fs::remove_dir_all(&path)?,
                Undo::Created(path) => remove_if_present(&path)?,
                Undo::Removed(path, bytes) => restore(&path, &bytes)?,
                Undo::Renamed(from, to, replaced) => {
                    if to.exists() {
                        std::fs::rename(&to, &from)?;
                    }
                    if let Some(bytes) = replaced {
                        restore(&to, &bytes)?;
                    }
                }
                Undo::Added(path, replaced) => {
                    remove_if_present(&path)?;
                    if let Some(bytes) = replaced {
                        restore(&path, &bytes)?;
                    }
                }
                Undo::MovedDir(from, to, _) => {
                    if to.exists() {
                        std::fs::rename(&to, &from)?;
                    }
                }
            }
        }
        if let Some((path, offset, bytes)) = torn.filter(|(path, ..)| path.exists()) {
            let file = std::fs::OpenOptions::new().write(true).open(&path)?;
            let end = offset + bytes.len() as u64;
            if file.metadata()?.len() < end {
                file.set_len(end)?;
            }
            write_at(&file, offset, &bytes)?;
        }
        state.crashed = false;
        state.crash_at = None;
        Ok(())
    }

    /// before records what a power loss would take back of a change, or fails it with the fault planned for it.
    fn before(&self, change: &Change<'_>) -> std::io::Result<()> {
        let mut state = locked(&self.state);
        if state.crashed {
            return Err(std::io::Error::other("the simulated store crashed"));
        }
        state.calls += 1;
        let line = self.traced(change);
        state.trace.push(line);
        let call = state.calls;
        if state.crash_at == Some(call) {
            state.crashed = true;
            return Err(std::io::Error::other("the simulated store crashed"));
        }
        if state.full_at.contains(&call) {
            return Err(std::io::Error::new(ErrorKind::StorageFull, "the simulated disk is full"));
        }
        if let Change::Sync(path) = *change
            && state.deferred_at.contains(&call)
        {
            state.dropped.insert(path.to_path_buf());
            return Err(std::io::Error::new(ErrorKind::StorageFull, "the simulated disk is full"));
        }
        match *change {
            Change::Write(path, offset, len) => {
                keep_durable(&mut state, path);
                state.last_write = Some((path.to_path_buf(), offset, Vec::with_capacity(len)));
            }
            Change::Truncate(path) => keep_durable(&mut state, path),
            Change::Create(path) => match path.exists() {
                true => keep_durable(&mut state, path),
                false => {
                    state.durable.insert(path.to_path_buf(), None);
                    state.undo.push(Undo::Created(path.to_path_buf()));
                }
            },
            Change::Remove(path) => {
                let bytes = durable_bytes(&state, path)?;
                state.durable.remove(path);
                state.dropped.remove(path);
                state.undo.push(Undo::Removed(path.to_path_buf(), bytes));
            }
            Change::Rename(from, to) => {
                let replaced = match to.is_file() {
                    true => Some(durable_bytes(&state, to)?),
                    false => None,
                };
                let across = from.parent() != to.parent();
                let undo = match (across, from.is_dir()) {
                    (false, _) => vec![Undo::Renamed(from.to_path_buf(), to.to_path_buf(), replaced)],
                    (true, true) => {
                        let parents = [from, to].into_iter().map(|path| host::directory_of(path).to_path_buf());
                        vec![Undo::MovedDir(from.to_path_buf(), to.to_path_buf(), parents.collect())]
                    }
                    (true, false) => vec![
                        Undo::Removed(from.to_path_buf(), durable_bytes(&state, from)?),
                        Undo::Added(to.to_path_buf(), replaced),
                    ],
                };
                let renamed = |path: &Path| {
                    let rest = path.strip_prefix(from).ok()?;
                    Some(if rest.as_os_str().is_empty() { to.to_path_buf() } else { to.join(rest) })
                };
                let moved: Vec<PathBuf> = state.dropped.iter().filter(|path| path.starts_with(from)).cloned().collect();
                for path in moved {
                    state.dropped.remove(&path);
                    state.dropped.extend(renamed(&path));
                }
                let moved: Vec<PathBuf> = state.durable.keys().filter(|path| path.starts_with(from)).cloned().collect();
                for path in moved {
                    let durable = state.durable.remove(&path).unwrap_or_default();
                    let renamed = to.join(path.strip_prefix(from).unwrap_or(Path::new("")));
                    state.durable.insert(if path == from { to.to_path_buf() } else { renamed }, durable);
                }
                state.undo.extend(undo);
            }
            Change::CreateDir(dir) if !dir.exists() => state.undo.push(Undo::Created(dir.to_path_buf())),
            Change::CreateDir(_) => {}
            Change::Sync(path) => {
                if !state.dropped.contains(path) {
                    state.durable.remove(path);
                }
                if state.last_write.as_ref().is_some_and(|(written, ..)| written == path) {
                    state.last_write = None;
                }
            }
            Change::SyncDir(dir) => {
                let mut kept: Vec<Undo> = Vec::new();
                for mut undo in std::mem::take(&mut state.undo) {
                    let durable = match &mut undo {
                        Undo::MovedDir(_, _, unsynced) => {
                            unsynced.retain(|parent| parent != dir);
                            unsynced.is_empty()
                        }
                        undo => undo_in(undo, dir),
                    };
                    match (durable, &undo) {
                        (false, _) => kept.push(undo),
                        (true, Undo::Renamed(from, to, _) | Undo::MovedDir(from, to, _)) => {
                            for path in kept.iter_mut().flat_map(Undo::paths) {
                                if let Some(rest) =
                                    path.strip_prefix(from).ok().filter(|rest| !rest.as_os_str().is_empty())
                                {
                                    *path = to.join(rest);
                                }
                            }
                        }
                        (true, _) => {}
                    }
                }
                state.undo = kept;
            }
        }
        Ok(())
    }

    /// wrote records the bytes of the last write, which a crash may leave in part.
    fn wrote(&self, path: &Path, bytes: &[u8]) {
        let mut state = locked(&self.state);
        let mut digest = DefaultHasher::new();
        digest.write(bytes);
        state.trace.push(format!("wrote {:016x}", digest.finish()));
        if let Some((_, _, kept)) = state.last_write.as_mut().filter(|(written, ..)| written == path) {
            kept.extend_from_slice(bytes);
        }
    }

    /// traced returns a door call as a line of the trace, with its paths under the simulated directory.
    fn traced(&self, change: &Change<'_>) -> String {
        let relative = |path: &Path| {
            let parts = path.strip_prefix(&self.root).unwrap_or(path).components();
            parts.map(|part| part.as_os_str().to_string_lossy()).collect::<Vec<_>>().join("/")
        };
        match *change {
            Change::Write(path, offset, len) => format!("write {} {offset} {len}", relative(path)),
            Change::Truncate(path) => format!("truncate {}", relative(path)),
            Change::Create(path) => format!("create {}", relative(path)),
            Change::Remove(path) => format!("remove {}", relative(path)),
            Change::Rename(from, to) => format!("rename {} {}", relative(from), relative(to)),
            Change::CreateDir(dir) => format!("create dir {}", relative(dir)),
            Change::Sync(path) => format!("sync {}", relative(path)),
            Change::SyncDir(dir) => format!("sync dir {}", relative(dir)),
        }
    }

    /// now returns the simulated time of day.
    pub(crate) fn now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH + EPOCH + self.tick()
    }

    /// instant returns a reading of the simulated monotonic clock.
    pub(crate) fn instant(&self) -> Instant {
        self.started + self.tick()
    }

    /// unique returns a number no other call of the simulation returns.
    pub(crate) fn unique(&self) -> u64 {
        let mut state = locked(&self.state);
        state.ids += 1;
        state.ids
    }

    /// tick moves the simulated clock a microsecond and returns its time since the start.
    fn tick(&self) -> Duration {
        let mut state = locked(&self.state);
        state.ticks += 1;
        Duration::from_micros(state.ticks)
    }
}

/// current returns the simulation the thread started, if one runs.
pub(crate) fn current() -> Option<Arc<Sim>> {
    let thread = std::thread::current().id();
    locked(&CURRENT).iter().find(|(started, _)| *started == thread).map(|(_, sim)| sim.clone())
}

/// adopt makes a simulation the running thread's, as an actor's thread shares its test's.
pub(crate) fn adopt(sim: Option<Arc<Sim>>) {
    if let Some(sim) = sim {
        let thread = std::thread::current().id();
        let mut current = locked(&CURRENT);
        current.retain(|(started, _)| *started != thread);
        current.push((thread, sim));
    }
}

/// before tells the simulation over a changed file's directory, if one runs, what a door is about to do, failing the
/// door with the fault it planned; a crash under a run stops the run's actors there.
pub(crate) fn before(change: Change<'_>) -> std::io::Result<()> {
    let path = match change {
        Change::Write(path, ..)
        | Change::Truncate(path)
        | Change::Create(path)
        | Change::Remove(path)
        | Change::Rename(_, path)
        | Change::CreateDir(path)
        | Change::Sync(path)
        | Change::SyncDir(path) => path,
    };
    let sim = locked(&SIMS).iter().find(|sim| path.starts_with(&sim.root)).cloned();
    let Some(sim) = sim else { return Ok(()) };
    let told = sim.before(&change);
    if told.is_err() && sim.crashed() {
        crate::run::crash();
    }
    told
}

/// cut returns the part of a write's bytes that a short write planned at the door call just made lets through, with the
/// error the write then fails with, or all of them.
pub(crate) fn cut<'a>(path: &Path, bytes: &'a [u8]) -> (&'a [u8], std::io::Result<()>) {
    let Some(sim) = locked(&SIMS).iter().find(|sim| path.starts_with(&sim.root)).cloned() else {
        return (bytes, Ok(()));
    };
    let mut state = locked(&sim.state);
    if !state.short_at.contains(&state.calls) {
        return (bytes, Ok(()));
    }
    let kept = (next(&mut state.random) % bytes.len().max(1) as u64) as usize;
    (&bytes[..kept], Err(std::io::Error::new(ErrorKind::StorageFull, "the simulated disk is full")))
}

/// wrote tells the simulation over a written file's directory, if one runs, the bytes a write wrote.
pub(crate) fn wrote(path: &Path, bytes: &[u8]) {
    let sim = locked(&SIMS).iter().find(|sim| path.starts_with(&sim.root)).cloned();
    if let Some(sim) = sim {
        sim.wrote(path, bytes);
    }
}

/// keep_durable keeps a file's durable bytes, as of its last sync, the first time it changes after it.
fn keep_durable(state: &mut State, path: &Path) {
    if !state.durable.contains_key(path) {
        let bytes = std::fs::read(path).ok();
        state.durable.insert(path.to_path_buf(), bytes);
    }
}

/// durable_bytes returns a file's bytes as of its last sync.
fn durable_bytes(state: &State, path: &Path) -> std::io::Result<Vec<u8>> {
    match state.durable.get(path) {
        Some(bytes) => Ok(bytes.clone().unwrap_or_default()),
        None => std::fs::read(path),
    }
}

/// undo_in reports whether an entry change is of a directory's entries.
fn undo_in(undo: &Undo, dir: &Path) -> bool {
    let path = match undo {
        Undo::Created(path) | Undo::Removed(path, _) | Undo::Added(path, _) => path,
        Undo::Renamed(_, to, _) | Undo::MovedDir(_, to, _) => to,
    };
    host::directory_of(path) == dir
}

/// restore writes a file back whole, making its directory first.
fn restore(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, bytes)
}

/// remove_if_present removes a file that may be gone already.
fn remove_if_present(path: &Path) -> std::io::Result<()> {
    match std::fs::remove_file(path) {
        Err(err) if err.kind() != ErrorKind::NotFound => Err(err),
        _ => Ok(()),
    }
}

/// write_at writes bytes at an offset of an open file.
fn write_at(file: &std::fs::File, offset: u64, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::{Seek, SeekFrom, Write};
    let mut file = file;
    file.seek(SeekFrom::Start(offset))?;
    file.write_all(bytes)
}

/// next returns the seeded generator's next number.
pub(crate) fn next(state: &mut u64) -> u64 {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    *state
}

#[cfg(test)]
mod tests {
    use super::*;
    use host::Open;
    use host::io::Io;

    /// Dir is a test's directory under the system's temporary one, removed when it drops.
    struct Dir(PathBuf);

    impl Dir {
        /// new makes an empty directory for a test.
        fn new(name: &str) -> Dir {
            let path = std::env::temp_dir().join(format!("sim-{name}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).unwrap();
            Dir(path)
        }
    }

    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    /// write_whole writes a new file whole and durably: created, written, synced, and its directory synced.
    fn write_whole(io: &mut Io, path: &Path, bytes: &[u8]) -> Result<(), host::Error> {
        let file = host::open(io, path, Open::CreateNew)?;
        host::write_at(io, &file, 0, bytes)?;
        host::sync(io, &file)?;
        host::sync_path(io, path.parent().unwrap(), true)
    }

    /// overwrite writes bytes at an offset of an existing file, without a sync.
    fn overwrite(io: &mut Io, path: &Path, offset: u64, bytes: &[u8]) -> Result<(), host::Error> {
        let file = host::open(io, path, Open::ReadWrite)?;
        host::write_at(io, &file, offset, bytes)
    }

    /// A crash keeps exactly what was synced: an unsynced write is taken back, and so is a file whose creation no sync
    /// of its directory made durable.
    #[test]
    fn a_crash_keeps_exactly_the_bytes_and_the_entries_that_were_synced() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("synced");
        let root = &dir.0;
        let sim = Sim::start(root, 1);
        let (kept, created) = (root.join("kept"), root.join("created"));
        write_whole(io, &kept, b"synced").unwrap();
        overwrite(io, &kept, 0, b"unsynced").unwrap();
        let file = host::open(io, &created, Open::CreateNew).unwrap();
        host::write_at(io, &file, 0, b"bytes").unwrap();
        host::sync(io, &file).unwrap();
        drop(file);
        sim.crash_at(sim.calls() + 1);
        assert!(host::sync_path(io, root, true).is_err() && sim.crashed());
        sim.recover(false).unwrap();
        assert_eq!(std::fs::read(&kept).unwrap(), b"synced");
        assert!(!created.exists());
        sim.stop();
    }

    /// A torn last write keeps a part of its bytes, and a full disk fails one call alone.
    #[test]
    fn a_torn_last_write_keeps_a_part_of_it_and_a_full_disk_fails_one_call() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("torn");
        let root = &dir.0;
        let sim = Sim::start(root, 7);
        let path = root.join("torn");
        write_whole(io, &path, b"0123456789").unwrap();
        overwrite(io, &path, 2, b"abcdef").unwrap();
        sim.crash_at(sim.calls() + 1);
        assert!(host::sync_path(io, root, true).is_err());
        sim.recover(true).unwrap();
        let bytes = std::fs::read(&path).unwrap();
        let kept = (0..=6).find(|&kept| bytes == [&b"01"[..], &b"abcdef"[..kept], &b"23456789"[kept..]].concat());
        assert!(kept.is_some(), "{bytes:?}");
        sim.full_at(sim.calls() + 1);
        let full = write_whole(io, &root.join("full"), b"full").unwrap_err();
        assert_eq!(full.sqlstate(), b"53100");
        write_whole(io, &root.join("again"), b"full").unwrap();
        sim.stop();
    }

    /// A rename is durable only once its directory's sync is, so a crash at that sync takes it back, carrying the
    /// synced bytes to the old name.
    #[test]
    fn a_rename_a_crash_takes_back_carries_the_synced_bytes_to_the_old_name() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("rename");
        let root = &dir.0;
        let sim = Sim::start(root, 5);
        let (a, b) = (root.join("a"), root.join("b"));
        write_whole(io, &a, b"synced").unwrap();
        overwrite(io, &a, 0, b"UNSYNC").unwrap();
        sim.crash_at(sim.calls() + 2);
        assert!(host::rename_durably(io, &a, &b).is_err() && sim.crashed());
        sim.recover(false).unwrap();
        assert_eq!(std::fs::read(&a).unwrap(), b"synced");
        assert!(!b.exists());
        host::rename_durably(io, &a, &b).unwrap();
        sim.crash_at(sim.calls() + 1);
        assert!(host::sync_path(io, root, true).is_err());
        sim.recover(false).unwrap();
        assert_eq!(std::fs::read(&b).unwrap(), b"synced");
        assert!(!a.exists());
        sim.stop();
    }

    /// A short write keeps a part of its bytes, and a failed sync keeps its writes from every later sync, as a kernel
    /// that drops the pages of a failed sync does.
    #[test]
    fn a_short_write_keeps_a_part_of_its_bytes_and_a_failed_sync_keeps_its_writes_from_every_later_sync() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("short");
        let root = &dir.0;
        let sim = Sim::start(root, 9);
        let path = root.join("file");
        write_whole(io, &path, b"0123456789").unwrap();
        let file = host::open(io, &path, Open::ReadWrite).unwrap();
        sim.short_at(sim.calls() + 1);
        let short = host::write_at(io, &file, 0, b"abcdefghij").unwrap_err();
        assert_eq!(short.sqlstate(), b"53100");
        let bytes = std::fs::read(&path).unwrap();
        let kept = (0..10).find(|&kept| bytes == [&b"abcdefghij"[..kept], &b"0123456789"[kept..]].concat());
        assert!(kept.is_some_and(|kept| kept < 10), "{bytes:?}");
        host::sync(io, &file).unwrap();
        host::write_at(io, &file, 0, b"ABCDEFGHIJ").unwrap();
        sim.deferred_at(sim.calls() + 1);
        assert!(host::sync(io, &file).is_err());
        host::write_at(io, &file, 0, b"after").unwrap();
        host::sync(io, &file).unwrap();
        drop(file);
        sim.recover(false).unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), bytes, "a power loss keeps the writes of a failed sync");
        sim.stop();
    }

    /// The host's errors carry the SQLSTATEs Postgres' file layer gives them: a missing file is 58P01, a file that
    /// exists is 58P02 to a creation, and a read past the end is a short read, XX001.
    #[test]
    fn the_hosts_errors_carry_postgres_sqlstates() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("errors");
        let path = dir.0.join("file");
        assert_eq!(host::open_to_read(io, &path).err().unwrap().sqlstate(), b"58P01");
        write_whole(io, &path, b"four").unwrap();
        assert_eq!(host::open(io, &path, Open::CreateNew).err().unwrap().sqlstate(), b"58P02");
        let file = host::open_to_read(io, &path).unwrap();
        let short = host::read_at(io, &file, 0, &mut [0; 8]).unwrap_err();
        assert_eq!(short.sqlstate(), b"XX001");
        assert!(short.message().ends_with("read 4 of 8"), "{:?}", short.message());
        assert_eq!(host::list_dir(io, &dir.0).unwrap(), ["file"]);
    }

    /// A removal whose directory's sync failed is finished by a retry that finds the file gone: the retry syncs the
    /// directory, so a crash after it doesn't bring the file back.
    #[test]
    fn a_removal_retried_after_its_sync_failed_makes_the_files_absence_durable() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("absent");
        let root = &dir.0;
        let sim = Sim::start(root, 11);
        let path = root.join("file");
        write_whole(io, &path, b"synced").unwrap();
        sim.full_at(sim.calls() + 2);
        assert!(host::remove_durably_if_there(io, &path).is_err() && !sim.crashed());
        assert!(!path.exists());
        assert!(!host::remove_durably_if_there(io, &path).unwrap());
        sim.crash_at(sim.calls() + 1);
        assert!(host::sync_path(io, root, true).is_err());
        sim.recover(false).unwrap();
        assert!(!path.exists(), "a removal its retry finished came back after a crash");
        sim.stop();
    }

    /// A long loop's check finds the cancel a test planned at its nth check, and the ones before it find none.
    #[test]
    fn a_planned_interrupt_stops_the_check_it_was_planned_at() {
        crate::install();
        let io = &mut Io::testing();
        crate::interrupt_at(2);
        assert!(io.check().is_ok() && io.check().is_ok());
        assert_eq!(io.check().unwrap_err().sqlstate(), b"57014");
        assert!(io.check().is_ok());
    }

    /// A fault in a door that tidies up after a failure is a change that didn't happen, and no error: the leftover
    /// stays, and a rename taken back answers that it didn't take, whether its rename or its sync failed.
    #[test]
    fn a_fault_in_a_door_that_tidies_up_is_a_change_that_did_not_happen() {
        crate::install();
        let io = &mut Io::testing();
        let dir = Dir::new("tidy");
        let root = &dir.0;
        let sim = Sim::start(root, 12);
        let (file, back) = (root.join("file"), root.join("back"));
        write_whole(io, &file, b"left").unwrap();
        sim.full_at(sim.calls() + 1);
        host::remove_leftover(io, &file);
        assert!(file.exists());
        sim.full_at(sim.calls() + 1);
        assert!(!host::rename_back(io, &file, &back) && file.exists() && !back.exists());
        sim.full_at(sim.calls() + 2);
        assert!(!host::rename_back(io, &file, &back) && back.exists());
        host::remove_leftover(io, &back);
        assert!(!back.exists() && !sim.crashed());
        sim.stop();
    }
}
