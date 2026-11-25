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

//! `SimHost`, `host`'s seams over the real file system for tests: each seam that changes a file tells the simulation
//! over its directory first (`faults`), and its errors carry the SQLSTATEs and wording Postgres' file layer gives them.
//! Tests take their directories from `tempdir`.
#![expect(
    clippy::disallowed_methods,
    clippy::disallowed_types,
    reason = "the simulation's host is the real file system, clock, and process the server's host reaches through Postgres, for tests alone"
)]

use std::any::{Any, TypeId};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CStr;
use std::fs::{File, OpenOptions};
use std::io::{ErrorKind, Read, Seek, SeekFrom, Write};
use std::marker::PhantomData;
use std::num::NonZeroU32;
use std::ops::{Deref, DerefMut};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard, Once, PoisonError};
use std::thread::ThreadId;
use std::time::{Duration, Instant, SystemTime};

use host::cache::{Bytes, Found, Key, Placement};
use host::delta::directory::{Opened, Owner};
use host::delta::{Leaving, Page, Size, WordOp};
use host::io::Io;
use host::local::BackendLocal;
use host::shared::{Kind, Looked, Op};
use host::{
    Command, CompressDictionary, DecompressDictionary, Error, Host, Lsn, MayRaise, Open, ReadHost, Row, Status,
    Storage, WalKind, Xid,
};

use crate::faults::{self, Change};
use crate::{cache, delta, shared};

/// BACKUPS is the simulated server's count of running base backups, the collector text's §3.3 meeting point's.
static BACKUPS: AtomicU64 = AtomicU64::new(0);

/// COLLECTING is the simulated server's collection word, set while a collection runs.
static COLLECTING: AtomicU64 = AtomicU64::new(0);

/// HELD is how many holds of the process's interrupts are live, inside a run or out of one.
static HELD: AtomicU64 = AtomicU64::new(0);

/// NAPPED is how long the simulated server's naps asked to wait in all, which pass at once.
static NAPPED: Mutex<Duration> = Mutex::new(Duration::ZERO);

/// DISK is the simulated volume's free bytes and its size, a terabyte free of two until a test sets them.
static DISK: Mutex<(u64, u64)> = Mutex::new((1 << 40, 2 << 40));

/// SimHost is the simulation's host.
pub struct SimHost;

/// HOST is the process's simulation host.
static HOST: SimHost = SimHost;

/// INSTALL installs the host once, whichever test runs first.
static INSTALL: Once = Once::new();

/// install installs the simulation's host in the test process, if no test has yet.
pub fn install() {
    INSTALL.call_once(|| host::install(&HOST).expect("no host is installed in a test process yet"));
}

/// TEST is the thread whose test holds the process, with how many holds of it are live.
static TEST: Mutex<Option<(ThreadId, usize)>> = Mutex::new(None);

/// ENDED wakes the tests waiting for the process once a test's last hold drops.
static ENDED: Condvar = Condvar::new();

/// Process holds the test process for one test's thread, so that one backend, or one run of actors, uses the process
/// and its shared memory at a time, as the server runs one backend per process; the thread may take several holds,
/// and the drop of its last starts shared memory and the resource owners afresh, so the next test finds nothing of
/// this one's, and fails the test whose owners still remember an owned record, which its transaction's end releases.
#[derive(Debug)]
pub struct Process(PhantomData<*const ()>);

impl Drop for Process {
    fn drop(&mut self) {
        let left = {
            let mut test = TEST.lock().unwrap_or_else(PoisonError::into_inner);
            let Some((_, live)) = test.as_mut() else { return };
            *live -= 1;
            if *live != 0 {
                return;
            }
            *test = None;
            let left = cache::left();
            shared::reset();
            DATA.lock().unwrap_or_else(PoisonError::into_inner).take();
            ENDED.notify_all();
            left
        };
        if !left.is_empty() && !std::thread::panicking() {
            panic!("the test ended with owned records of the kinds {left:?} that no transaction's end released");
        }
    }
}

/// DATA is the test's data directory, made at its first use and removed when the test's last hold of the process
/// drops.
static DATA: Mutex<Option<TempDir>> = Mutex::new(None);

/// napped returns how long the naps asked to wait since the last call, and starts the count again.
pub fn napped() -> Duration {
    std::mem::take(&mut *NAPPED.lock().unwrap_or_else(PoisonError::into_inner))
}

/// set_disk_space sets the simulated volume's free bytes and its size, for a test of the collector's low disk.
pub fn set_disk_space(free: u64, total: u64) {
    *DISK.lock().unwrap_or_else(PoisonError::into_inner) = (free, total);
}

/// data_dir returns the test's data directory, which holds `pg_versioned` as initdb makes it, making both at its
/// first use.
pub fn data_dir() -> PathBuf {
    let mut data = DATA.lock().unwrap_or_else(PoisonError::into_inner);
    let dir = data.get_or_insert_with(|| {
        let dir = tempdir().expect("the system's temporary directory takes a directory");
        std::fs::create_dir(dir.path().join(host::VERSIONED)).expect("a new directory takes a directory");
        dir
    });
    dir.path().to_path_buf()
}

/// process holds the test process for the running thread's test, waiting while another thread's test holds it.
pub fn process() -> Process {
    let thread = std::thread::current().id();
    let mut test = TEST.lock().unwrap_or_else(PoisonError::into_inner);
    loop {
        match test.as_mut() {
            None => *test = Some((thread, 1)),
            Some((holder, live)) if *holder == thread => *live += 1,
            Some(_) => {
                test = ENDED.wait(test).unwrap_or_else(PoisonError::into_inner);
                continue;
            }
        }
        return Process(PhantomData);
    }
}

/// TestIo is a test's capability, which holds the process for its test.
pub struct TestIo {
    io: Io,
    /// process is the test's hold of the process.
    _process: Process,
}

impl Deref for TestIo {
    type Target = Io;

    fn deref(&self) -> &Io {
        &self.io
    }
}

impl DerefMut for TestIo {
    fn deref_mut(&mut self) -> &mut Io {
        &mut self.io
    }
}

/// io installs the simulation's host, if no test has yet, and returns a test's capability, which holds the process.
pub fn io() -> TestIo {
    install();
    TestIo { io: Io::testing(), _process: process() }
}

/// FILES maps each open file's number to the file and its path, which the simulation names changes by.
static FILES: Mutex<BTreeMap<i32, (File, PathBuf)>> = Mutex::new(BTreeMap::new());

/// SCRATCH are the numbers of the open scratch files, each deleted when it closes.
static SCRATCH: Mutex<BTreeSet<i32>> = Mutex::new(BTreeSet::new());

/// scratch_files returns how many scratch files are open, for a test of what a writer's drop leaves.
pub fn scratch_files() -> usize {
    SCRATCH.lock().unwrap_or_else(PoisonError::into_inner).len()
}

/// collecting returns the collection word, for a test that a failed collection ends what it began.
pub fn collecting() -> u64 {
    COLLECTING.load(Ordering::SeqCst)
}

/// interrupts_held returns how many holds of the process's interrupts are live, for a test that a failure gives
/// each back.
pub fn interrupts_held() -> u64 {
    HELD.load(Ordering::SeqCst)
}

/// NEXT_FILE is the next file number.
static NEXT_FILE: AtomicI32 = AtomicI32::new(1);

/// NEXT_UNIQUE is the next of the process's unique numbers, outside a simulation.
static NEXT_UNIQUE: AtomicU64 = AtomicU64::new(0);

/// WAL_END is the position past the last record inserted.
static WAL_END: AtomicU64 = AtomicU64::new(0);

/// WINDOWS is, by thread, its open checkpoint window with the names of the pauses its open loaded.
static WINDOWS: Mutex<Vec<(ThreadId, Vec<&'static CStr>)>> = Mutex::new(Vec::new());

/// NEW_CIDS is every record of heap's `XLOG_HEAP2_NEW_CID` inserted, its storage, its row, its command, and its end.
static NEW_CIDS: Mutex<Vec<(Storage, Row, host::NewCid, Lsn)>> = Mutex::new(Vec::new());

/// new_cids returns every record of heap's `XLOG_HEAP2_NEW_CID` the work inserted, in its order.
pub fn new_cids() -> Vec<(Storage, Row, host::NewCid, Lsn)> {
    NEW_CIDS.lock().unwrap_or_else(PoisonError::into_inner).clone()
}

/// LOGGED is every record inserted, its kind's value, its body, and its end, which a test reads back as redo would.
static LOGGED: Mutex<Vec<(u8, Vec<u8>, Lsn)>> = Mutex::new(Vec::new());

/// LAST_REDO is the redo point of the last checkpoint a test completed (`checkpointed`).
static LAST_REDO: AtomicU64 = AtomicU64::new(0);

/// checkpointed records a checkpoint completed with a redo point, which `last_redo` then returns.
pub fn checkpointed(redo: Lsn) {
    LAST_REDO.store(redo, Ordering::Relaxed);
}

/// SESSION_DATABASE is the database a backend is connected to, by its id, 0 for none, and its tablespace.
static SESSION_DATABASE: BackendLocal<(AtomicU32, AtomicU32)> = BackendLocal::new();

/// connect connects the running backend to a database, by its id and the tablespace of its own directory, which
/// `session_database` then returns.
pub fn connect(database: NonZeroU32, tablespace: u32) {
    let session = SESSION_DATABASE.get();
    session.0.store(database.get(), Ordering::Relaxed);
    session.1.store(tablespace, Ordering::Relaxed);
}

/// KEPT_WAL_READ is the range of the WAL kept for a fallback that a check last read, if one did since it was taken.
static KEPT_WAL_READ: Mutex<Option<(Lsn, Lsn)>> = Mutex::new(None);

/// WARNINGS are the reports at WARNING the work made since they were taken, each its SQLSTATE and its text.
static WARNINGS: Mutex<Vec<([u8; 5], String)>> = Mutex::new(Vec::new());

/// take_warnings takes the reports at WARNING the work made since they were last taken.
pub fn take_warnings() -> Vec<([u8; 5], String)> {
    std::mem::take(&mut *WARNINGS.lock().unwrap_or_else(PoisonError::into_inner))
}

/// LOGS are the reports at LOG the work made since they were taken.
static LOGS: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// take_logs takes the reports at LOG the work made since they were last taken.
pub fn take_logs() -> Vec<String> {
    std::mem::take(&mut *LOGS.lock().unwrap_or_else(PoisonError::into_inner))
}

/// kept_wal_read takes the range of the WAL kept for a fallback that a check last read, if one did.
pub fn kept_wal_read() -> Option<(Lsn, Lsn)> {
    KEPT_WAL_READ.lock().unwrap_or_else(PoisonError::into_inner).take()
}

/// RECORD_HEADER is the size of a record's header, Postgres' `SizeOfXLogRecord` with its main data's length.
const RECORD_HEADER: u64 = 29;

/// ACTORS is, by thread, the actor the thread runs as.
static ACTORS: Mutex<Vec<(ThreadId, u64)>> = Mutex::new(Vec::new());

/// Acting is a thread's run as an actor, which ends when it drops, the thread's earlier actor, if any, running again.
struct Acting {
    /// thread is the thread that runs as the actor.
    thread: ThreadId,
    /// before is the actor the thread ran as before, if any.
    before: Option<u64>,
}

impl Drop for Acting {
    fn drop(&mut self) {
        let mut acting = ACTORS.lock().unwrap_or_else(PoisonError::into_inner);
        acting.retain(|(thread, _)| *thread != self.thread);
        acting.extend(self.before.map(|actor| (self.thread, actor)));
    }
}

/// running_actor returns the actor the running thread runs as, if any.
pub(crate) fn running_actor() -> Option<u64> {
    let thread = std::thread::current().id();
    let acting = ACTORS.lock().unwrap_or_else(PoisonError::into_inner);
    acting.iter().find(|(actor_thread, _)| *actor_thread == thread).map(|&(_, actor)| actor)
}

/// as_actor runs a body as an actor, a backend of the simulation's whose backend-local state is its own, named by a
/// number below the ids a run's actors take.
pub fn as_actor<R>(actor: u64, body: impl FnOnce() -> R) -> R {
    assert!(actor < crate::run::FIRST_ACTOR, "a test names actors below the ids a run's actors take");
    acting_as(actor, body)
}

/// acting_as runs a body as an actor of any id.
pub(crate) fn acting_as<R>(actor: u64, body: impl FnOnce() -> R) -> R {
    let thread = std::thread::current().id();
    let before = {
        let mut acting = ACTORS.lock().unwrap_or_else(PoisonError::into_inner);
        let before = acting.iter().position(|(acting, _)| *acting == thread).map(|at| acting.remove(at).1);
        acting.push((thread, actor));
        before
    };
    let _acting = Acting { thread, before };
    body()
}

/// NEXT_DIR is the next number a temporary directory of the process tries.
static NEXT_DIR: AtomicU64 = AtomicU64::new(0);

/// TempDir is a test's directory, removed with everything in it when dropped.
pub struct TempDir(PathBuf);

impl TempDir {
    /// path returns the directory's path.
    pub fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// tempdir makes a new empty directory under the system's temporary directory, named for the process and a number no
/// other in it takes.
pub fn tempdir() -> std::io::Result<TempDir> {
    loop {
        let number = NEXT_DIR.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!("doltgres-sim-{}-{number}", std::process::id()));
        match std::fs::create_dir(&path) {
            Err(error) if error.kind() == ErrorKind::AlreadyExists => {}
            made => return made.map(|()| TempDir(path)),
        }
    }
}

/// OUT_OF_MEMORY is whether the next decompression runs out of memory, which a test of what an allocation failure
/// does sets.
static OUT_OF_MEMORY: AtomicBool = AtomicBool::new(false);

/// out_of_memory_at_next_decompress makes the next decompression fail with Postgres' out of memory error.
pub fn out_of_memory_at_next_decompress() {
    OUT_OF_MEMORY.store(true, Ordering::Relaxed);
}

/// INTERRUPT_AT is, by thread, the number of interrupt checks after which one finds a cancel pending.
static INTERRUPT_AT: Mutex<Vec<(ThreadId, u64)>> = Mutex::new(Vec::new());

/// interrupt_at makes the check after `checks` more on this thread find a cancel pending, as a test's stop of a long
/// loop does.
pub fn interrupt_at(checks: u64) {
    let thread = std::thread::current().id();
    let mut planned = INTERRUPT_AT.lock().unwrap_or_else(PoisonError::into_inner);
    planned.retain(|(planner, _)| *planner != thread);
    planned.push((thread, checks));
}

/// interrupt_due reports whether a thread's planned interrupt is due at its next check.
pub(crate) fn interrupt_due(thread: ThreadId) -> bool {
    INTERRUPT_AT
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .iter()
        .any(|&(planner, at)| planner == thread && at == 0)
}

/// checked fails with Postgres' cancel once the running thread's planned interrupt is due, counting the check down
/// otherwise; while a run's actor holds a shared lock, the epoch lock, or a checkpoint-delay window, the check waits,
/// as Postgres holds interrupts there.
pub(crate) fn checked() -> Result<(), Error> {
    if crate::run::holding() {
        return Ok(());
    }
    let thread = std::thread::current().id();
    let mut planned = INTERRUPT_AT.lock().unwrap_or_else(PoisonError::into_inner);
    let Some(at) = planned.iter().position(|(planner, _)| *planner == thread) else { return Ok(()) };
    match planned[at].1 {
        0 => {
            planned.remove(at);
            Err(Error::fork(b"57014", "canceling statement due to user request".to_owned()))
        }
        _ => {
            planned[at].1 -= 1;
            Ok(())
        }
    }
}

/// files returns the table of open files.
fn files() -> MutexGuard<'static, BTreeMap<i32, (File, PathBuf)>> {
    FILES.lock().unwrap_or_else(PoisonError::into_inner)
}

/// failed returns Postgres' error for a file operation that failed, with the SQLSTATE `errcode_for_file_access` gives
/// the system's error and the action's wording.
fn failed(action: &str, error: &std::io::Error) -> Error {
    let sqlstate = match error.kind() {
        ErrorKind::NotFound => b"58P01",
        ErrorKind::AlreadyExists => b"58P02",
        ErrorKind::PermissionDenied => b"42501",
        ErrorKind::StorageFull | ErrorKind::QuotaExceeded => b"53100",
        _ => b"58030",
    };
    Error::fork(sqlstate, format!("{action}: {error}"))
}

/// opened returns an open file's handle and path, which every seam on a file looks up.
fn opened(file: i32) -> (File, PathBuf) {
    let files = files();
    let (handle, path) = files.get(&file).expect("a seam names only a file `host::open` opened");
    (handle.try_clone().expect("an open file's handle clones"), path.clone())
}

/// told tells the simulation over a change's directory what a seam is about to do, failing the seam with its fault.
fn told(change: Change<'_>, action: impl FnOnce() -> String) -> Result<(), Error> {
    faults::before(change).map_err(|error| failed(&action(), &error))
}

/// removed removes a directory and everything in it, telling the simulation of each file's removal, and reports
/// whether all of it went: a file whose removal the simulation fails or the file system refuses stays, with the
/// directories above it.
fn removed(path: &Path) -> bool {
    let Ok(entries) = std::fs::read_dir(path) else { return false };
    let mut all = true;
    for entry in entries {
        let Ok(entry) = entry else { return false };
        let child = entry.path();
        all &= match entry.file_type() {
            Ok(kind) if kind.is_dir() => removed(&child),
            _ => {
                let action = || format!("could not remove file \"{}\"", child.display());
                told(Change::Remove(&child), action).is_ok() && std::fs::remove_file(&child).is_ok()
            }
        };
    }
    all && std::fs::remove_dir(path).is_ok()
}

/// opening returns what an open of a file was doing, for its error.
fn opening(path: &Path) -> String {
    format!("could not open file \"{}\"", path.display())
}

/// open_as opens a file as a seam asks, telling the simulation of a file it creates, and returns the operating
/// system's answer, which the seam turns into its error or its value.
fn open_as(path: &Path, how: Open) -> Result<std::io::Result<File>, Error> {
    Ok(match how {
        Open::ReadWrite => OpenOptions::new().read(true).write(true).open(path),
        Open::CreateNew => {
            if !path.exists() {
                told(Change::Create(path), || opening(path))?;
            }
            OpenOptions::new().read(true).write(true).create_new(true).open(path)
        }
    })
}

/// numbered gives an opened file its number, or nothing for the one failure a seam answers as a value.
fn numbered(path: &Path, opened: std::io::Result<File>, answered: ErrorKind) -> Result<Option<i32>, Error> {
    match opened {
        Ok(handle) => {
            let number = NEXT_FILE.fetch_add(1, Ordering::Relaxed);
            files().insert(number, (handle, path.to_path_buf()));
            Ok(Some(number))
        }
        Err(error) if error.kind() == answered => Ok(None),
        Err(error) => Err(failed(&opening(path), &error)),
    }
}

/// listing returns what a listing of a directory was doing, for its error.
fn listing(path: &Path) -> String {
    format!("could not open directory \"{}\"", path.display())
}

/// listed returns a directory's names in byte order.
fn listed(path: &Path, entries: std::fs::ReadDir) -> Result<Vec<String>, Error> {
    let mut names = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|error| failed(&listing(path), &error))?;
        names.push(entry.file_name().to_string_lossy().into_owned());
    }
    names.sort();
    Ok(names)
}

impl ReadHost for SimHost {
    fn open_to_read(&self, _: &MayRaise, path: &Path) -> Result<i32, Error> {
        crate::run::yield_point();
        let handle = File::open(path).map_err(|error| failed(&opening(path), &error))?;
        let number = NEXT_FILE.fetch_add(1, Ordering::Relaxed);
        files().insert(number, (handle, path.to_path_buf()));
        Ok(number)
    }

    fn open_if_there(&self, _: &MayRaise, path: &Path) -> Result<Option<i32>, Error> {
        crate::run::yield_point();
        numbered(path, File::open(path), ErrorKind::NotFound)
    }

    fn read_at(&self, _: &MayRaise, file: i32, offset: u64, buffer: &mut [u8]) -> Result<(), Error> {
        crate::run::yield_point();
        let (mut handle, path) = opened(file);
        let action = || format!("could not read file \"{}\"", path.display());
        handle.seek(SeekFrom::Start(offset)).map_err(|error| failed(&action(), &error))?;
        let mut done = 0;
        while done < buffer.len() {
            match handle.read(&mut buffer[done..]) {
                Ok(0) => {
                    let message = format!("{}: read {done} of {}", action(), buffer.len());
                    return Err(Error::fork(b"XX001", message));
                }
                Ok(n) => done += n,
                Err(error) if error.kind() == ErrorKind::Interrupted => {}
                Err(error) => return Err(failed(&action(), &error)),
            }
        }
        Ok(())
    }

    fn size(&self, _: &MayRaise, file: i32) -> Result<u64, Error> {
        crate::run::yield_point();
        let (handle, path) = opened(file);
        let action = || format!("could not seek to end of file \"{}\"", path.display());
        handle.metadata().map(|meta| meta.len()).map_err(|error| failed(&action(), &error))
    }

    fn prefetch(&self, _: i32, _: u64, _: u64) {}
    fn close(&self, file: i32) {
        crate::run::yield_point();
        let closed = files().remove(&file);
        if SCRATCH.lock().unwrap_or_else(PoisonError::into_inner).remove(&file)
            && let Some((handle, path)) = closed
        {
            drop(handle);
            let _ = std::fs::remove_file(path);
        }
    }

    fn tablespace_root(&self, tablespace: u32) -> PathBuf {
        match tablespace {
            host::PG_DEFAULT => data_dir(),
            other => data_dir().join("pg_tblspc").join(other.to_string()).join("PG_19_SIM"),
        }
    }

    fn list_dir(&self, _: &MayRaise, path: &Path) -> Result<Vec<String>, Error> {
        crate::run::yield_point();
        let entries = std::fs::read_dir(path).map_err(|error| failed(&listing(path), &error))?;
        listed(path, entries)
    }

    fn list_dir_if_there(&self, _: &MayRaise, path: &Path) -> Result<Option<Vec<String>>, Error> {
        crate::run::yield_point();
        match std::fs::read_dir(path) {
            Ok(entries) => listed(path, entries).map(Some),
            Err(error) if matches!(error.kind(), ErrorKind::NotFound | ErrorKind::NotADirectory) => Ok(None),
            Err(error) => Err(failed(&listing(path), &error)),
        }
    }

    fn prepare_decompressing(&self, bytes: &[u8]) -> Result<DecompressDictionary, Error> {
        let prepared = pg::zstd::DecompressDictionary::new(bytes).ok_or_else(Error::out_of_memory)?;
        Ok(DecompressDictionary::new(Box::new(prepared)))
    }

    fn frame_size(&self, frame: &[u8]) -> Option<u64> {
        pg::zstd::frame_size(frame)
    }

    fn decompress(
        &self,
        frame: &[u8],
        size: usize,
        dictionary: Option<&DecompressDictionary>,
    ) -> Result<Option<Vec<u8>>, Error> {
        if OUT_OF_MEMORY.swap(false, Ordering::Relaxed) {
            return Err(Error::out_of_memory());
        }
        match pg::zstd::decompress(frame, size, dictionary.map(|dictionary| prepared(dictionary.prepared()))) {
            Ok(bytes) => Ok(Some(bytes)),
            Err(pg::zstd::Failed::Damaged) => Ok(None),
            Err(pg::zstd::Failed::OutOfMemory) => Err(Error::out_of_memory()),
        }
    }
}

impl Host for SimHost {
    fn now(&self) -> SystemTime {
        crate::run::yield_point();
        faults::current().map_or_else(SystemTime::now, |sim| sim.now())
    }

    fn instant(&self) -> Instant {
        crate::run::yield_point();
        faults::current().map_or_else(Instant::now, |sim| sim.instant())
    }

    fn process_id(&self) -> u32 {
        std::process::id()
    }

    fn work_mem(&self) -> u64 {
        4096 * 1024
    }

    fn session_database(&self) -> Option<(NonZeroU32, u32)> {
        let session = SESSION_DATABASE.get();
        NonZeroU32::new(session.0.load(Ordering::Relaxed)).map(|database| (database, session.1.load(Ordering::Relaxed)))
    }

    fn exiting(&self) -> bool {
        crate::run::exiting()
    }

    fn unique(&self) -> u64 {
        crate::run::yield_point();
        faults::current().map_or_else(|| NEXT_UNIQUE.fetch_add(1, Ordering::Relaxed), |sim| sim.unique())
    }

    fn check_interrupts(&self, _: &MayRaise) -> Result<(), Error> {
        crate::run::yield_point();
        checked()
    }

    fn hold_interrupts(&self) {
        HELD.fetch_add(1, Ordering::SeqCst);
        crate::run::hold(true);
    }

    fn resume_interrupts(&self) {
        HELD.fetch_sub(1, Ordering::SeqCst);
        crate::run::hold(false);
    }

    fn open(&self, _: &MayRaise, path: &Path, how: Open) -> Result<i32, Error> {
        crate::run::yield_point();
        let handle = open_as(path, how)?.map_err(|error| failed(&opening(path), &error))?;
        let number = NEXT_FILE.fetch_add(1, Ordering::Relaxed);
        files().insert(number, (handle, path.to_path_buf()));
        Ok(number)
    }

    fn create_if_absent(&self, _: &MayRaise, path: &Path) -> Result<Option<i32>, Error> {
        crate::run::yield_point();
        numbered(path, open_as(path, Open::CreateNew)?, ErrorKind::AlreadyExists)
    }

    fn open_scratch(&self, _: &MayRaise) -> Result<i32, Error> {
        crate::run::yield_point();
        let number = NEXT_FILE.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!("doltgres-sim-{}-scratch-{number}", std::process::id()));
        let opened = OpenOptions::new().read(true).write(true).create_new(true).open(&path);
        let handle = opened.map_err(|error| failed("could not create temporary file", &error))?;
        files().insert(number, (handle, path));
        SCRATCH.lock().unwrap_or_else(PoisonError::into_inner).insert(number);
        Ok(number)
    }

    fn write_at(&self, _: &MayRaise, file: i32, offset: u64, bytes: &[u8]) -> Result<(), Error> {
        crate::run::yield_point();
        let (mut handle, path) = opened(file);
        let action = || format!("could not write to file \"{}\"", path.display());
        told(Change::Write(&path, offset, bytes.len()), action)?;
        let (kept, cut) = faults::cut(&path, bytes);
        handle.seek(SeekFrom::Start(offset)).map_err(|error| failed(&action(), &error))?;
        handle.write_all(kept).map_err(|error| failed(&action(), &error))?;
        faults::wrote(&path, kept);
        cut.map_err(|error| failed(&action(), &error))
    }

    fn truncate(&self, _: &MayRaise, file: i32, len: u64) -> Result<(), Error> {
        crate::run::yield_point();
        let (handle, path) = opened(file);
        let action = || format!("could not truncate file \"{}\"", path.display());
        told(Change::Truncate(&path), action)?;
        handle.set_len(len).map_err(|error| failed(&action(), &error))
    }

    fn sync(&self, _: &MayRaise, file: i32) -> Result<(), Error> {
        crate::run::yield_point();
        let (_, path) = opened(file);
        told(Change::Sync(&path), || format!("could not fsync file \"{}\"", path.display()))
    }

    fn rename(&self, _: &MayRaise, from: &Path, to: &Path) -> Result<(), Error> {
        crate::run::yield_point();
        let action = || format!("could not rename file \"{}\" to \"{}\"", from.display(), to.display());
        told(Change::Rename(from, to), action)?;
        std::fs::rename(from, to).map_err(|error| failed(&action(), &error))
    }

    fn rename_durably(&self, _: &MayRaise, from: &Path, to: &Path) -> Result<(), Error> {
        crate::run::yield_point();
        let action = || format!("could not rename file \"{}\" to \"{}\"", from.display(), to.display());
        told(Change::Rename(from, to), action)?;
        std::fs::rename(from, to).map_err(|error| failed(&action(), &error))?;
        let sync_dir =
            |dir: &Path| told(Change::SyncDir(dir), || format!("could not fsync file \"{}\"", dir.display()));
        sync_dir(host::directory_of(to))?;
        match host::directory_of(from) == host::directory_of(to) {
            true => Ok(()),
            false => sync_dir(host::directory_of(from)),
        }
    }

    fn remove_durably(&self, _: &MayRaise, path: &Path) -> Result<(), Error> {
        crate::run::yield_point();
        let action = || format!("could not remove file \"{}\"", path.display());
        told(Change::Remove(path), action)?;
        std::fs::remove_file(path).map_err(|error| failed(&action(), &error))?;
        told(Change::SyncDir(host::directory_of(path)), || {
            format!("could not fsync file \"{}\"", host::directory_of(path).display())
        })
    }

    fn remove_durably_if_there(&self, _: &MayRaise, path: &Path) -> Result<bool, Error> {
        crate::run::yield_point();
        let action = || format!("could not remove file \"{}\"", path.display());
        let there = path.symlink_metadata().is_ok();
        if there {
            told(Change::Remove(path), action)?;
            std::fs::remove_file(path).map_err(|error| failed(&action(), &error))?;
        }
        told(Change::SyncDir(host::directory_of(path)), || {
            format!("could not fsync file \"{}\"", host::directory_of(path).display())
        })?;
        Ok(there)
    }

    fn remove_leftover(&self, path: &Path) {
        crate::run::yield_point();
        let action = || format!("could not remove file \"{}\"", path.display());
        if path.symlink_metadata().is_ok() && told(Change::Remove(path), action).is_ok() {
            let _ = std::fs::remove_file(path);
        }
    }

    fn remove_tree(&self, _: &MayRaise, path: &Path) -> Result<bool, Error> {
        crate::run::yield_point();
        match path.symlink_metadata() {
            Err(error) if error.kind() == ErrorKind::NotFound => Ok(true),
            _ => Ok(removed(path)),
        }
    }

    fn rename_back(&self, from: &Path, to: &Path) -> bool {
        crate::run::yield_point();
        let action = || format!("could not rename file \"{}\" to \"{}\"", from.display(), to.display());
        let sync_dir =
            |dir: &Path| told(Change::SyncDir(dir), || format!("could not fsync file \"{}\"", dir.display())).is_ok();
        from.symlink_metadata().is_ok()
            && told(Change::Rename(from, to), action).is_ok()
            && std::fs::rename(from, to).is_ok()
            && sync_dir(host::directory_of(to))
            && (host::directory_of(from) == host::directory_of(to) || sync_dir(host::directory_of(from)))
    }

    fn sync_path(&self, _: &MayRaise, path: &Path, directory: bool) -> Result<(), Error> {
        crate::run::yield_point();
        let change = if directory { Change::SyncDir(path) } else { Change::Sync(path) };
        told(change, || format!("could not fsync file \"{}\"", path.display()))
    }

    fn make_dir(&self, _: &MayRaise, path: &Path) -> Result<(), Error> {
        crate::run::yield_point();
        let action = || format!("could not create directory \"{}\"", path.display());
        told(Change::CreateDir(path), action)?;
        match std::fs::create_dir(path) {
            Err(error) if error.kind() != ErrorKind::AlreadyExists => Err(failed(&action(), &error)),
            _ => Ok(()),
        }
    }

    fn disk_space(&self, _: &MayRaise, _: &Path) -> Result<(u64, u64), Error> {
        crate::run::yield_point();
        Ok(*DISK.lock().unwrap_or_else(PoisonError::into_inner))
    }

    fn tree_bytes(&self, raise: &MayRaise, path: &Path) -> Result<u64, Error> {
        crate::run::yield_point();
        let metadata = match std::fs::metadata(path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == ErrorKind::NotFound => return Ok(0),
            Err(error) => return Err(failed(&listing(path), &error)),
        };
        if !metadata.is_dir() {
            return Ok(metadata.len());
        }
        let mut total = 0;
        for name in self.list_dir_if_there(raise, path)?.into_iter().flatten() {
            total += self.tree_bytes(raise, &path.join(name))?;
        }
        Ok(total)
    }

    fn nap(&self, _: &MayRaise, duration: Duration) -> Result<(), Error> {
        crate::run::yield_point();
        *NAPPED.lock().unwrap_or_else(PoisonError::into_inner) += duration;
        Ok(())
    }

    fn wal_insert(&self, _: &MayRaise, kind: WalKind, body: &[u8]) -> Result<Lsn, Error> {
        crate::run::yield_point();
        let len = RECORD_HEADER + u64::try_from(body.len()).expect("a record's body fits a position");
        let end = WAL_END.fetch_add(len, Ordering::Relaxed) + len;
        LOGGED.lock().unwrap_or_else(PoisonError::into_inner).push((kind.value(), body.to_vec(), end));
        Ok(end)
    }

    fn wal_new_cid(&self, _: &MayRaise, storage: &Storage, tid: Row, cid: host::NewCid) -> Result<Lsn, Error> {
        crate::run::yield_point();
        let end = WAL_END.fetch_add(RECORD_HEADER, Ordering::Relaxed) + RECORD_HEADER;
        NEW_CIDS.lock().unwrap_or_else(PoisonError::into_inner).push((*storage, tid, cid, end));
        Ok(end)
    }

    fn wal_flush(&self, _: &MayRaise, _: Lsn) -> Result<(), Error> {
        crate::run::yield_point();
        Ok(())
    }

    fn fold_point(&self, _: &MayRaise) -> Result<Lsn, Error> {
        crate::run::yield_point();
        Ok(WAL_END.load(Ordering::Relaxed))
    }

    fn checkpoint_now(&self, _: &MayRaise) -> Result<(), Error> {
        crate::run::yield_point();
        LAST_REDO.store(WAL_END.load(Ordering::Relaxed), Ordering::Relaxed);
        Ok(())
    }

    fn quiet_database(&self, _: &MayRaise, _: u32) -> Result<(), Error> {
        crate::run::yield_point();
        Ok(())
    }

    fn unquiet_database(&self, _: &MayRaise, _: u32) -> Result<(), Error> {
        crate::run::yield_point();
        Ok(())
    }

    fn last_redo(&self, _: &MayRaise) -> Result<Lsn, Error> {
        crate::run::yield_point();
        Ok(LAST_REDO.load(Ordering::Relaxed))
    }

    fn warn(&self, _: &MayRaise, error: Error, context: &CStr) -> Result<(), Error> {
        crate::run::yield_point();
        let text = format!("{}: {}", context.to_string_lossy(), error.message());
        WARNINGS.lock().unwrap_or_else(PoisonError::into_inner).push((*error.sqlstate(), text));
        Ok(())
    }

    fn log(&self, _: &MayRaise, message: &CStr) -> Result<(), Error> {
        crate::run::yield_point();
        LOGS.lock().unwrap_or_else(PoisonError::into_inner).push(message.to_string_lossy().into_owned());
        Ok(())
    }

    fn refuse_recovery(&self, _: &str) {
        unreachable!("the simulation never recovers")
    }

    fn wal_stand(&self) -> host::WalStand {
        host::WalStand {
            recovering: false,
            insertion_timeline: 1,
            flushed: Lsn::MAX,
            replayed: 0,
            replayed_timeline: 0,
            target_timeline: 1,
        }
    }

    fn warn_kept_wal(&self, _: &MayRaise, from: Lsn, to: Lsn, _: u32, _: Lsn) -> Result<(), Error> {
        crate::run::yield_point();
        *KEPT_WAL_READ.lock().unwrap_or_else(PoisonError::into_inner) = Some((from, to));
        Ok(())
    }

    fn next_fork_record(&self, _: &MayRaise, from: Lsn, kinds: u32) -> Result<Option<host::ForkRecord>, Error> {
        crate::run::yield_point();
        let logged = LOGGED.lock().unwrap_or_else(PoisonError::into_inner);
        let found =
            logged.iter().filter(|&&(kind, _, end)| end > from && kinds & 1 << kind != 0).min_by_key(|record| record.2);
        Ok(found.map(|(kind, body, end)| (*kind, *end, body.clone())))
    }

    fn delay_checkpoints(&self, _: &MayRaise, pauses: &[&'static CStr]) -> Result<(), Error> {
        crate::run::yield_point();
        crate::run::hold(true);
        let thread = std::thread::current().id();
        WINDOWS.lock().unwrap_or_else(PoisonError::into_inner).push((thread, pauses.to_vec()));
        Ok(())
    }

    fn xid_status(&self, _: &MayRaise, xid: Xid) -> Result<Status, Error> {
        crate::run::yield_point();
        crate::transactions::status(xid)
    }

    fn xid_in_progress(&self, _: &MayRaise, xid: Xid) -> Result<bool, Error> {
        crate::run::yield_point();
        crate::transactions::in_progress(xid)
    }

    fn xids_answered(&self) -> bool {
        crate::transactions::answered()
    }

    fn removable_in(&self, _: &MayRaise, snapshot: &dyn Any, xid: Xid) -> Result<bool, Error> {
        crate::run::yield_point();
        let taken = snapshot.downcast_ref::<crate::Taken>().expect("the simulation makes a snapshot only from its own");
        Ok(taken.removable(xid))
    }

    fn current_writer(&self, _: &MayRaise) -> Result<(Xid, Xid), Error> {
        crate::run::yield_point();
        Ok(crate::transactions::current_writer())
    }

    fn current_command(&self, _: &MayRaise, used: bool) -> Result<Command, Error> {
        crate::run::yield_point();
        Ok(Command(crate::transactions::current_command(used)))
    }

    fn xid_is_current(&self, xid: Xid) -> bool {
        crate::run::yield_point();
        crate::transactions::is_current(xid)
    }

    fn combo_command(&self, _: &MayRaise, _: Xid, cmin: Command, cmax: Command) -> Result<Command, Error> {
        crate::run::yield_point();
        Ok(Command(crate::transactions::combo_command(cmin.0, cmax.0)))
    }

    fn resume_checkpoints(&self, _: bool) {
        let thread = std::thread::current().id();
        WINDOWS.lock().unwrap_or_else(PoisonError::into_inner).retain(|(opener, _)| *opener != thread);
        crate::run::yield_point();
        crate::run::hold(false);
    }

    fn actor(&self) -> Option<u64> {
        running_actor()
    }

    fn yield_point(&self) {
        crate::run::yield_point();
    }

    fn pause(&self, _: &MayRaise, point: &CStr) -> Result<(), Error> {
        let thread = std::thread::current().id();
        let unloaded = WINDOWS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .iter()
            .any(|(opener, loaded)| *opener == thread && !loaded.contains(&point));
        if unloaded {
            return Err(Error::fork(b"XX000", format!("a window's pause {point:?} wasn't loaded at its open")));
        }
        crate::run::pause(point);
        Ok(())
    }

    fn injected(&self, _: &MayRaise, _: &CStr) -> Result<bool, Error> {
        Ok(false)
    }

    fn at_exit(&self, work: TypeId, run: fn(&mut Io) -> Result<(), Error>) -> Result<(), Error> {
        crate::run::at_exit(work, run)
    }

    fn shared_make(&self, _: &MayRaise, kind: &Kind) -> Result<Option<u64>, Error> {
        crate::run::yield_point();
        shared::make(kind)
    }

    fn shared_publish(&self, place: u64) -> u64 {
        shared::publish(place)
    }

    fn shared_claim(&self, kind: &Kind, location: u64) -> Result<Option<u64>, Error> {
        crate::run::yield_point();
        shared::claim(kind, location)
    }

    fn shared_free(&self, place: u64) {
        shared::free(place);
    }

    fn shared_give_up(&self, place: u64) {
        shared::give_up(place);
    }

    fn shared_open(&self, _: &MayRaise, kind: &Kind, location: u64) -> Result<Option<u64>, Error> {
        crate::run::yield_point();
        shared::open(kind, location)
    }

    fn shared_close(&self, place: u64) {
        shared::close(place);
    }

    fn shared_reclaim(&self, kind: &Kind, location: u64) -> Result<Looked, Error> {
        crate::run::yield_point();
        shared::reclaim(kind, location)
    }

    fn shared_reclaimed(&self, place: u64) -> bool {
        shared::reclaimed(place)
    }

    fn shared_unreclaim(&self, place: u64) {
        shared::unreclaim(place);
    }

    fn shared_sweep(&self, _: &MayRaise, kind: &Kind, atomic: usize) -> Result<Vec<(u64, u64)>, Error> {
        shared::sweep(kind, atomic)
    }

    fn shared_single(&self, kind: &Kind) -> Result<u64, Error> {
        shared::single(kind)
    }

    fn shared_ready(&self) -> bool {
        true
    }

    fn shared_lock(&self, _: &MayRaise, place: u64, lock: usize, exclusive: bool) -> Result<(), Error> {
        crate::run::yield_point();
        shared::lock(place, lock, exclusive)?;
        crate::run::hold(true);
        Ok(())
    }

    fn shared_unlock(&self, place: u64, lock: usize) {
        crate::run::yield_point();
        shared::unlock(place, lock);
        crate::run::hold(false);
    }

    fn key_lock(&self, _: &MayRaise, stripes: &[u32]) -> Result<(), Error> {
        crate::run::yield_point();
        shared::key_lock(stripes)?;
        crate::run::hold(true);
        Ok(())
    }

    fn key_count(&self, stripe: u32) -> u64 {
        shared::key_count(stripe)
    }

    fn key_hold(&self, stripe: u32) -> bool {
        shared::key_hold(stripe)
    }

    fn key_release(&self, stripe: u32) {
        shared::key_release(stripe);
    }

    fn key_held_wait(&self, _: &MayRaise, stripe: u32) -> Result<(), Error> {
        shared::key_held_wait(stripe)
    }

    fn key_unlock(&self, stripes: &[u32], pushed: bool) {
        crate::run::yield_point();
        shared::key_unlock(stripes, pushed);
        crate::run::hold(false);
    }

    fn xact_wait(&self, _: &MayRaise, xid: Xid, _: Option<host::Operation>, wait: bool) -> Result<bool, Error> {
        loop {
            crate::run::yield_point();
            if crate::transactions::status(xid)? != Status::InProgress {
                return Ok(true);
            }
            if !wait {
                return Ok(false);
            }
            if !crate::run::wait(crate::run::Part::Ended { xid: xid.0 }) {
                panic!(
                    "the simulation runs one actor at a time, so a wait for another actor's transaction would never end"
                );
            }
        }
    }

    fn speculative_wait(&self, raise: &MayRaise, xid: Xid, _: u32) -> Result<(), Error> {
        self.xact_wait(raise, xid, None, true).map(drop)
    }

    fn lock_tuple(&self, _: &MayRaise, tuple: host::TupleLock, wait: bool) -> Result<bool, Error> {
        crate::run::yield_point();
        shared::lock_tuple(tuple, wait)
    }

    fn unlock_tuple(&self, tuple: host::TupleLock) {
        shared::unlock_tuple(tuple);
    }

    fn flag_take(&self, _: &MayRaise, place: u64, flag: usize) -> Result<(), Error> {
        crate::run::yield_point();
        shared::flag_take(place, flag)
    }

    fn flag_try(&self, _: &MayRaise, place: u64, flag: usize) -> Result<bool, Error> {
        crate::run::yield_point();
        shared::flag_try(place, flag)
    }

    fn flag_clear(&self, place: u64, flag: usize) {
        crate::run::yield_point();
        shared::flag_clear(place, flag);
    }

    fn shared_atomics(&self, place: u64) -> usize {
        shared::atomics(place)
    }

    fn atomic(&self, place: u64, atomic: usize, op: Op) -> u64 {
        crate::run::yield_point();
        shared::atomic(place, atomic, op)
    }

    fn blob_read(&self, place: u64, blob: usize) -> Vec<u8> {
        shared::blob_read(place, blob)
    }

    fn blob_write(&self, place: u64, blob: usize, bytes: &[u8]) -> Result<(), Error> {
        shared::blob_write(place, blob, bytes)
    }

    fn entry_read(&self, place: u64, array: usize, entry: usize) -> Result<Vec<u8>, Error> {
        shared::entry_read(place, array, entry)
    }

    fn entry_write(&self, place: u64, array: usize, entry: usize, bytes: &[u8]) -> Result<(), Error> {
        shared::entry_write(place, array, entry, bytes)
    }

    fn shared_location(&self, place: u64) -> u64 {
        shared::location(place)
    }

    fn shared_live(&self, location: u64) -> bool {
        shared::live(location)
    }

    fn shared_gone(&self, kind: &Kind, location: u64) -> Result<bool, Error> {
        shared::gone(kind, location)
    }

    fn slots_name(&self, place: u64, array: usize) -> u64 {
        crate::run::yield_point();
        shared::slots_name(place, array)
    }

    fn slots_rename(&self, place: u64, array: usize, loaded: u64, name: u64) -> bool {
        crate::run::yield_point();
        shared::slots_rename(place, array, loaded, name)
    }

    fn slots_empty(&self, place: u64, array: usize) {
        crate::run::yield_point();
        shared::slots_empty(place, array);
    }

    fn slots_claim(&self, _: &MayRaise, place: u64, array: usize, bytes: &[u8]) -> Result<bool, Error> {
        crate::run::yield_point();
        Ok(shared::slots_claim(place, array, bytes))
    }

    fn slots_read(&self, place: u64, array: usize) -> Vec<Vec<u8>> {
        shared::slots_read(place, array)
    }

    fn words_mine(&self, kind: &Kind) -> Result<u64, Error> {
        shared::mine(kind)
    }

    fn word_store(&self, place: u64, word: usize, value: u64) {
        crate::run::yield_point();
        shared::word_store(place, word, value);
    }

    fn word_store_own(&self, _: usize, kind: &Kind, word: usize, value: u64) {
        crate::run::yield_point();
        shared::word_store(shared::mine(kind).expect("a kind of words has one form"), word, value);
    }

    fn word_load(&self, kind: &Kind, proc: u32, word: usize) -> Result<u64, Error> {
        crate::run::yield_point();
        shared::word_load(kind, proc, word)
    }

    fn words_place(&self, kind: &Kind, proc: u32) -> Result<u64, Error> {
        crate::run::yield_point();
        shared::words_place(kind, proc)
    }

    fn words_processes(&self, kind: &Kind) -> Result<Vec<u32>, Error> {
        crate::run::yield_point();
        shared::processes(kind)
    }

    fn epochs_lock(&self, _: &MayRaise) -> Result<(), Error> {
        crate::run::yield_point();
        shared::epochs_lock()?;
        crate::run::hold(true);
        Ok(())
    }

    fn epochs_unlock(&self) {
        crate::run::yield_point();
        shared::epochs_unlock();
        crate::run::hold(false);
    }

    fn epochs_advance(&self) -> u64 {
        crate::run::yield_point();
        shared::epochs_advance()
    }

    fn epochs_oldest(&self) -> u64 {
        crate::run::yield_point();
        shared::epochs_oldest()
    }

    fn epochs_read(&self, _: &MayRaise) -> Result<u64, Error> {
        crate::run::yield_point();
        Ok(shared::epochs_read())
    }

    fn epochs_end(&self, epoch: u64) {
        crate::run::yield_point();
        delta::ended(epoch);
    }

    fn proc_number(&self) -> u32 {
        shared::proc()
    }

    fn delta_hold(&self, _: &MayRaise) -> Result<u64, Error> {
        Ok(delta::hold())
    }

    fn delta_release(&self, epoch: u64) {
        delta::release(epoch);
    }

    fn delta_open(&self, name: Page, epoch: u64) -> Result<Opened, Error> {
        delta::open(name, epoch)
    }

    fn delta_bytes(&self, opened: &Opened, offset: u64, len: u64) -> Result<Bytes, Error> {
        delta::bytes(opened, offset, len)
    }

    fn delta_room(&self, opened: &Opened) -> u64 {
        delta::room(opened)
    }

    fn delta_filled(&self, opened: &Opened) -> u64 {
        delta::filled(opened)
    }

    fn delta_word(&self, body: u64, index: u64, op: WordOp) -> u64 {
        delta::word(body, index, op)
    }

    fn delta_alloc(&self, _: &MayRaise, sizes: &[Size], names: &mut Vec<Page>) -> Result<Option<u64>, Error> {
        delta::alloc(sizes, names)
    }

    fn delta_alloc_replacing(
        &self,
        _: &MayRaise,
        size: Size,
        replaced: Page,
        names: &mut Vec<Page>,
    ) -> Result<Option<u64>, Error> {
        delta::alloc_replacing(size, replaced, names)
    }

    fn delta_give_back(&self, name: Page) {
        delta::give_back(name);
    }

    fn delta_owned_append(&self, name: Page, record: &[u8]) -> Result<u32, Error> {
        delta::owned_append(name, record)
    }

    fn delta_owned_store(&self, name: Page, index: u64, value: u64) -> Result<(), Error> {
        delta::owned_store(name, index, value)
    }

    fn delta_link(&self, name: Page, owner: Owner) {
        delta::link(name, owner);
    }

    fn delta_append(&self, opened: &Opened, owner: Owner, record: &[u8]) -> Result<u32, Error> {
        delta::append(opened, owner, record)
    }

    fn delta_seal(&self, opened: &Opened, owner: Owner) -> Result<(), Error> {
        delta::seal(opened, owner)
    }

    fn delta_retire(&self, records: &[Leaving], names: &[Page]) {
        delta::retire(records, names);
    }

    fn delta_make(&self, _: &MayRaise, kind: &Kind, name: &str) -> Result<Result<(u64, u64), u64>, Error> {
        delta::make(kind, name)
    }

    fn delta_record(&self, kind: &Kind, location: u64, epoch: u64) -> Result<u64, Error> {
        delta::record(kind, location, epoch)
    }

    fn delta_publish(&self, place: u64) {
        delta::publish(place);
    }

    fn delta_unmake(&self, place: u64) {
        delta::unmake(place);
    }

    fn delta_pass(&self) {
        delta::pass();
    }

    fn delta_advanced(&self) -> (u64, u64) {
        let passed = crate::shared::epochs_take();
        crate::shared::epochs_advance();
        crate::shared::epochs_unlock();
        (passed, crate::shared::epochs_oldest())
    }

    fn delta_used(&self) -> u64 {
        delta::used()
    }

    fn delta_budget(&self) -> u64 {
        delta::budget()
    }

    fn delta_seed(&self) -> [u8; 32] {
        delta::seed()
    }

    fn backup_begin(&self, _: &MayRaise) -> Result<(), Error> {
        BACKUPS.fetch_add(1, Ordering::SeqCst);
        while COLLECTING.load(Ordering::SeqCst) != 0 {
            crate::run::yield_point();
        }
        Ok(())
    }

    fn backup_end(&self) {
        BACKUPS.fetch_sub(1, Ordering::SeqCst);
    }

    fn backups_running(&self) -> bool {
        BACKUPS.load(Ordering::SeqCst) != 0
    }

    fn collection_begin(&self) -> bool {
        COLLECTING.fetch_add(1, Ordering::SeqCst);
        if BACKUPS.load(Ordering::SeqCst) == 0 {
            return true;
        }
        COLLECTING.fetch_sub(1, Ordering::SeqCst);
        false
    }

    fn collection_end(&self) {
        COLLECTING.fetch_sub(1, Ordering::SeqCst);
    }

    fn collection_ask(&self, _: &MayRaise, _: u32) -> Result<[u64; 3], Error> {
        unreachable!("no simulation runs the collector worker, which answers an ask")
    }

    fn collection_asked(&self) -> Option<(u64, u32)> {
        None
    }

    fn collection_answer(&self, _: u64, _: [u64; 3]) {
        unreachable!("no simulation runs the collector worker, which answers an ask")
    }

    fn collector_started(&self) {
        unreachable!("no simulation runs the collector worker")
    }

    fn database_locked(&self, _: &MayRaise, _: u32) -> Result<bool, Error> {
        Ok(true)
    }

    fn database_unlocked(&self, _: u32) {}

    fn database_lock_waited(&self, _: &MayRaise, _: u32) -> Result<bool, Error> {
        Ok(false)
    }

    fn delta_wait(&self, _: &MayRaise, frees: u64) -> Result<(), Error> {
        delta::wait(frees)
    }

    fn cache_get(&self, _: &MayRaise, placement: Placement, key: &Key) -> Result<Option<Found>, Error> {
        crate::run::yield_point();
        Ok(cache::get(placement, key.bytes()))
    }

    fn cache_install(
        &self,
        _: &MayRaise,
        placement: Placement,
        key: &Key,
        len: usize,
        fill: &mut dyn FnMut(&mut [u8]),
    ) -> Result<Option<Found>, Error> {
        crate::run::yield_point();
        Ok(cache::install(placement, key.bytes(), len, fill))
    }

    fn cache_contains(&self, _: &MayRaise, placement: Placement, key: &Key) -> Result<bool, Error> {
        crate::run::yield_point();
        if cache::refused() {
            return Err(Error::fork(b"XX000", "the simulation refused the cache's lock".to_owned()));
        }
        Ok(cache::contains(placement, key.bytes()))
    }

    fn cache_unpin(&self, placement: Placement, frame: u32) {
        cache::unpin(placement, frame);
    }

    fn cache_make_local(&self, _: &MayRaise, bytes: usize) -> Result<(), Error> {
        cache::make_local(bytes);
        Ok(())
    }

    fn owner_enlarge(&self, _: &MayRaise) -> Result<u64, Error> {
        Ok(cache::owner_current())
    }

    fn owner_top_enlarge(&self, _: &MayRaise) -> Result<u64, Error> {
        Ok(cache::owner_top())
    }

    fn owner_remember(&self, owner: u64, pin: u64) {
        cache::owner_remember(owner, pin);
    }

    fn owner_forget(&self, owner: u64, pin: u64) {
        cache::owner_forget(owner, pin);
    }

    fn owned_remember(&self, owner: u64, record: u64, release: fn(u32)) {
        cache::owned_remember(owner, record, release);
    }

    fn owned_forget(&self, owner: u64, record: u64) {
        cache::owned_forget(owner, record);
    }

    /// btree_compare stands for a class's comparison function as the comparator's cases use one: function 0 raises,
    /// function 1 finds every two values equal, and any other orders its values' bytes in reverse, so that its order
    /// differs from the keys' byte order.
    fn btree_compare(&self, _: &MayRaise, compared: &host::Compared, a: &[u8], b: &[u8]) -> Result<i32, Error> {
        match compared.function {
            0 => Err(Error::fork(b"42883", "the simulation's comparison function 0 raises".to_owned())),
            1 => Ok(0),
            _ => Ok(b.cmp(a) as i32),
        }
    }

    /// sort_key stands for a collation's sort key as access's key tests' Reversed does: the text's bytes reversed.
    fn sort_key(&self, _: &MayRaise, _: u32, text: &[u8]) -> Result<Vec<u8>, Error> {
        Ok(text.iter().rev().copied().collect())
    }

    /// reformed refuses a fold's re-form through the catalog, which the simulation has none of: the fold's cases re-form
    /// their rows through their own `Reform`.
    fn reformed(&self, _: &MayRaise, _: &host::ReformRow<'_>) -> Result<host::ReformedRow, Error> {
        Err(Error::fork(b"XX000", "the simulation re-forms no row through the catalog".to_owned()))
    }

    fn prepare_compressing(&self, bytes: &[u8], level: i32) -> Result<CompressDictionary, Error> {
        let prepared = pg::zstd::CompressDictionary::new(bytes, level).ok_or_else(Error::out_of_memory)?;
        Ok(CompressDictionary::new(Box::new(prepared)))
    }

    fn compress(&self, bytes: &[u8], level: i32, dictionary: Option<&CompressDictionary>) -> Result<Vec<u8>, Error> {
        let prepared = dictionary.map(|dictionary| prepared(dictionary.prepared()));
        pg::zstd::compress(bytes, level, prepared).ok_or_else(Error::out_of_memory)
    }

    fn train(&self, samples: &[&[u8]], capacity: usize) -> Option<Vec<u8>> {
        pg::zstd::train(samples, capacity)
    }
}

/// prepared returns a dictionary as `pg` prepared it, which is how this host prepares every dictionary.
fn prepared<T: 'static>(dictionary: &(dyn std::any::Any + Send + Sync)) -> &T {
    dictionary.downcast_ref().expect("every dictionary is one this host prepared")
}

/// logged returns every record the simulation's processes inserted so far, in their order, each its kind's value, its
/// body, and its end, for a test of redo.
pub fn logged() -> Vec<(u8, Vec<u8>, Lsn)> {
    LOGGED.lock().unwrap_or_else(PoisonError::into_inner).clone()
}

#[cfg(test)]
mod tests {
    use super::*;
    use host::local::BackendLocal;

    /// A database's versioned files are in `pg_versioned` in the test's data directory for `pg_default` and in a
    /// tablespace's version directory for any other, the directory named by the database's id; the data directory
    /// is one for a test, holds `pg_versioned`, and goes with the test's hold of the process.
    #[test]
    fn a_databases_versioned_files_are_in_the_data_directory_or_a_tablespaces_version_directory() {
        let test_io = io();
        let data = data_dir();
        assert!(data.join("pg_versioned").is_dir());
        assert_eq!(host::tablespace_dir(5, host::PG_DEFAULT), data.join("pg_versioned").join("5"));
        let other = data.join("pg_tblspc").join("16400").join("PG_19_SIM").join("pg_versioned").join("5");
        assert_eq!(host::tablespace_dir(5, 16400), other);
        assert_eq!(data_dir(), data);
        drop(test_io);
        assert!(!data.exists());
    }

    /// Each actor keeps its own value of a backend-local static, apart from the process's, and a thread that runs as
    /// one actor inside another's run is the outer actor again once the inner run ends.
    #[test]
    fn each_actor_keeps_its_own_backend_local_value() {
        install();
        static COUNT: BackendLocal<AtomicU64> = BackendLocal::new();
        let count = || COUNT.get().load(Ordering::Relaxed);
        COUNT.get().fetch_add(1, Ordering::Relaxed);
        as_actor(1, || COUNT.get().fetch_add(10, Ordering::Relaxed));
        as_actor(2, || {
            COUNT.get().fetch_add(100, Ordering::Relaxed);
            as_actor(1, || assert_eq!(count(), 10));
            assert_eq!(count(), 100);
        });
        assert_eq!((count(), as_actor(1, count), as_actor(2, count)), (1, 10, 100));
    }

    /// The doors that ask whether a file or directory is there answer in their value, and raise every other failure.
    #[test]
    fn the_doors_that_ask_whether_something_is_there_answer_in_their_value() {
        let _process = crate::process();
        install();
        let io = &mut host::io::Io::testing();
        let dir = std::env::temp_dir().join(format!("doltgres-sim-doors-{}", std::process::id()));
        std::fs::create_dir(&dir).unwrap();
        let (file, missing) = (dir.join("file"), dir.join("missing"));
        assert!(host::open_if_there(io, &file).unwrap().is_none());
        let made = host::create_if_absent(io, &file).unwrap().expect("no file had the path");
        host::write_at(io, &made, 0, b"there").unwrap();
        assert!(host::create_if_absent(io, &file).unwrap().is_none());
        let opened = host::open_if_there(io, &file).unwrap().expect("the file is there");
        let mut read = [0; 5];
        host::read_at(io, &opened, 0, &mut read).unwrap();
        assert_eq!(&read, b"there");
        assert_eq!(host::list_dir_if_there(io, &dir).unwrap(), Some(vec!["file".to_owned()]));
        assert_eq!(host::list_dir_if_there(io, &missing).unwrap(), None);
        assert!(host::remove_durably_if_there(io, &file).unwrap());
        assert!(!host::remove_durably_if_there(io, &file).unwrap());
        let refused = host::create_if_absent(io, &missing.join("file")).map(drop);
        assert_eq!(refused.unwrap_err().sqlstate(), b"58P01");
        std::fs::remove_dir(&dir).unwrap();
    }

    /// The doors that tidy up after a failure never fail: a leftover file is removed and one that isn't there is left
    /// alone, and a rename taken back answers whether it took.
    #[test]
    fn the_doors_that_tidy_up_after_a_failure_never_fail() {
        let _process = crate::process();
        install();
        let io = &mut host::io::Io::testing();
        let dir = std::env::temp_dir().join(format!("doltgres-sim-tidy-{}", std::process::id()));
        std::fs::create_dir(&dir).unwrap();
        let (file, back) = (dir.join("file"), dir.join("back"));
        drop(host::create_if_absent(io, &file).unwrap());
        assert!(host::rename_back(io, &file, &back) && back.exists() && !file.exists());
        assert!(!host::rename_back(io, &file, &back) && back.exists());
        host::remove_leftover(io, &back);
        assert!(!back.exists());
        host::remove_leftover(io, &back);
        std::fs::remove_dir(&dir).unwrap();
    }

    /// A tree is removed with everything in it, and a directory that isn't there answers that it is gone.
    #[test]
    fn a_tree_is_removed_with_everything_in_it() {
        let _process = crate::process();
        install();
        let io = &mut host::io::Io::testing();
        let dir = std::env::temp_dir().join(format!("doltgres-sim-tree-{}", std::process::id()));
        let inner = dir.join("inner");
        std::fs::create_dir_all(&inner).unwrap();
        drop(host::create_if_absent(io, &dir.join("file")).unwrap());
        drop(host::create_if_absent(io, &inner.join("file")).unwrap());
        assert!(host::remove_tree(io, &dir).unwrap() && !dir.exists());
        assert!(host::remove_tree(io, &dir).unwrap());
    }

    /// A fault in the removal of a tree's file leaves that file and the directories above it, and the removal
    /// answers that something was left; asked again with no fault, it removes the rest.
    #[test]
    fn a_fault_in_a_trees_removal_leaves_a_file_and_says_so() {
        let _process = crate::process();
        install();
        let io = &mut host::io::Io::testing();
        let root = std::env::temp_dir().join(format!("doltgres-sim-tree-fault-{}", std::process::id()));
        let (tree, inner) = (root.join("tree"), root.join("tree").join("inner"));
        std::fs::create_dir_all(&inner).unwrap();
        let sim = crate::Sim::start(&root, 12);
        drop(host::create_if_absent(io, &inner.join("left")).unwrap());
        sim.full_at(sim.calls() + 1);
        assert!(!host::remove_tree(io, &tree).unwrap());
        assert!(inner.join("left").exists());
        assert!(host::remove_tree(io, &tree).unwrap() && !tree.exists() && !sim.crashed());
        sim.stop();
        std::fs::remove_dir(&root).unwrap();
    }

    /// A scratch file reads back what was written to it, and its close deletes it.
    #[test]
    fn a_scratch_file_is_deleted_when_it_closes() {
        let mut test_io = io();
        let io = &mut *test_io;
        let before = scratch_files();
        let scratch = host::open_scratch(io).unwrap();
        host::write_at(io, &scratch, 0, b"scratch").unwrap();
        let mut read = [0; 7];
        host::read_at(io, &scratch, 0, &mut read).unwrap();
        assert_eq!(&read, b"scratch");
        let path = files()[&scratch.number()].1.clone();
        assert!(path.exists());
        assert_eq!(scratch_files(), before + 1);
        drop(scratch);
        assert!(!path.exists(), "a closed scratch file stayed");
        assert_eq!(scratch_files(), before);
    }

    /// The window's pauses (CommPF 5's ruling): inside a checkpoint window a pause whose name the window's open loaded
    /// runs and any other fails, and once the window ends a pause of any name runs again, as the server's cached form
    /// gives way to the plain one.
    #[test]
    fn a_pause_inside_a_window_runs_only_a_name_its_open_loaded() {
        let mut test_io = io();
        let io = &mut *test_io;
        let window = host::delay_checkpoint(io, &[c"doltgres-sim-loaded"]).unwrap();
        assert!(host::pause!(io, c"doltgres-sim-loaded").is_ok());
        assert!(host::pause!(io, c"doltgres-sim-unloaded").is_err_and(|error| error.sqlstate() == b"XX000"));
        window.finish();
        assert!(host::pause!(io, c"doltgres-sim-unloaded").is_ok());
    }
}
