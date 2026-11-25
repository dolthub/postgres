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

//! The seams the storage crates reach Postgres through (design item 14), as one trait, and the doors that call the
//! host installed for the process, the simulation's. The storage crates reach a seam only through these doors, so the
//! simulation runs the functions the server runs with only the seams swapped.
//! Its seams so far are the clock and the process (the time of day, a monotonic clock, the process id, and numbers no
//! other call in the process returns), the file door (positioned reads and writes, syncs, durable renames and
//! removals, directories), the WAL (inserting the fork's records, flushing, and holding off a checkpoint's start), the
//! check for a pending interrupt, zstd's compression with trained dictionaries, and the simulation's actor that runs,
//! whose backend-local state is its own (`local`). Its doors that can raise take an `Io`, the entry's capability
//! (`io`), and return an `Error` the entry raises, and its locks keep file I/O out from under the locks that don't
//! allow it (`lock`).

#![forbid(unsafe_code)]

use std::any::{Any, TypeId};
use std::borrow::Cow;
use std::ffi::CStr;
use std::marker::PhantomData;
use std::num::NonZeroU32;
use std::ops::Deref;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::time::{Duration, Instant, SystemTime};

pub mod cache;
pub mod delta;
pub mod io;
pub mod local;
pub mod lock;
pub mod owned;
pub mod shared;

use io::{Io, IoDoors, ReadDoors};

/// MayRaise is what a seam that can raise takes, which only the doors of this crate hand a host.
pub struct MayRaise(PhantomData<*const ()>);

impl MayRaise {
    /// of makes what a seam takes, for a door's call of a seam.
    pub(crate) fn of() -> MayRaise {
        MayRaise(PhantomData)
    }

    /// testing returns what a seam takes, for `simc`'s calls of the simulation's host from the C port's doors.
    #[cfg(feature = "testing")]
    pub fn testing() -> MayRaise {
        MayRaise(PhantomData)
    }
}

/// doors returns what a seam that can raise takes, for a door given the `Io` or a view of one.
fn doors(_: &mut impl ReadDoors) -> MayRaise {
    MayRaise::of()
}

/// Error is an error a seam returns, which travels up to the entry to be raised: the fork's own, with its SQLSTATE,
/// its message, and its hint when it has one.
pub struct Error {
    /// sqlstate is the error's SQLSTATE.
    sqlstate: [u8; 5],
    /// message is the error's message.
    message: Cow<'static, str>,
    /// hint is the error's hint, when it has one.
    hint: Option<&'static str>,
}

/// Forked is an error as simc hands it to C: its SQLSTATE, its message, and its hint when it has one.
pub type Forked = ([u8; 5], String, Option<&'static str>);

impl Error {
    /// fork makes the fork's own error, with its SQLSTATE and message in Postgres' wording.
    pub fn fork(sqlstate: &[u8; 5], message: String) -> Error {
        Error { sqlstate: *sqlstate, message: Cow::Owned(message), hint: None }
    }

    /// hinted makes the fork's own error with a hint, both in Postgres' wording.
    pub fn hinted(sqlstate: &[u8; 5], message: &'static str, hint: &'static str) -> Error {
        Error { sqlstate: *sqlstate, message: Cow::Borrowed(message), hint: Some(hint) }
    }

    /// out_of_memory is Postgres' error of an allocation that failed, which it makes without allocating.
    pub fn out_of_memory() -> Error {
        Error { sqlstate: *b"53200", message: Cow::Borrowed("out of memory"), hint: None }
    }

    /// sqlstate returns the error's SQLSTATE.
    pub fn sqlstate(&self) -> &[u8; 5] {
        &self.sqlstate
    }

    /// message returns the error's message.
    pub fn message(&self) -> &str {
        &self.message
    }

    /// hint returns the error's hint, and nothing for one with none.
    pub fn hint(&self) -> Option<&'static str> {
        self.hint
    }

    /// into_parts gives simc the error's SQLSTATE, message, and hint.
    pub fn into_parts(self) -> Forked {
        (self.sqlstate, self.message.into_owned(), self.hint)
    }
}

/// An error shows its SQLSTATE and, for the fork's own, its message, as a test's failure reports it.
impl std::fmt::Debug for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let sqlstate = String::from_utf8_lossy(&self.sqlstate);
        f.debug_struct("Error").field("sqlstate", &sqlstate).field("message", &self.message()).finish()
    }
}

/// File is an open file, which closes when it drops. Only `open` makes one, so a handle can't be forged, used after its
/// file closes, or closed twice, and an error between opening and closing a file can't leak it (design item 13).
pub struct File {
    /// number is the host's number for the file.
    number: i32,
    /// host is the host that opened the file, which closes it.
    host: &'static dyn ReadHost,
}

impl File {
    /// number returns the host's number for the file, which a test compares across openings.
    pub fn number(&self) -> i32 {
        self.number
    }
}

impl Drop for File {
    fn drop(&mut self) {
        self.host.close(self.number);
    }
}

/// ScratchFile is a temporary file of the transaction that opened it, as Postgres' own work files are: placed by
/// `temp_tablespaces`, counted by `temp_file_limit`, and deleted when it closes. It lives within the entry that opened
/// it, which the resource owner that would close it outlasts, so its drop closes it first.
pub struct ScratchFile<'io> {
    file: File,
    entry: PhantomData<(&'io (), *const ())>,
}

impl Deref for ScratchFile<'_> {
    type Target = File;

    fn deref(&self) -> &File {
        &self.file
    }
}

/// Open is how a file is opened to write.
pub enum Open {
    /// ReadWrite opens an existing file to read and write.
    ReadWrite,
    /// CreateNew creates a file that must not exist, to read and write.
    CreateNew,
}

/// Lsn is a position in Postgres' WAL.
pub type Lsn = u64;

/// WalStand is where the server's WAL stands: whether recovery is in progress; the insertion timeline, zero until
/// recovery sets it at its end; the flushed position, zero while recovery is in progress; replay's position and
/// timeline, zero before replay begins; and the timeline recovery reads toward.
pub struct WalStand {
    pub recovering: bool,
    pub insertion_timeline: u32,
    pub flushed: Lsn,
    pub replayed: Lsn,
    pub replayed_timeline: u32,
    pub target_timeline: u32,
}

/// Storage names a storage by its relfilenode's locator: its tablespace, its database, and its relation number.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Storage {
    pub tablespace: u32,
    pub database: u32,
    pub relation: u32,
}

/// Table is a table's object id.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Table(pub u64);

/// Xid is the 64-bit id of a transaction or a subtransaction, Postgres' `FullTransactionId`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Xid(pub u64);

/// Status is what Postgres says of a transaction or a subtransaction: still in progress, committed, or aborted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    InProgress,
    Committed,
    Aborted,
}

/// Command is a command's id within its transaction, Postgres' `CommandId`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Command(pub u32);

/// SnapshotKind is a snapshot's type, each of Postgres' `SnapshotType`s (seam 2).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotKind {
    Mvcc,
    /// Current is `SNAPSHOT_SELF`, which sees every committed change and the current command's own.
    Current,
    Any,
    Toast,
    Dirty,
    HistoricMvcc,
    NonVacuumable,
}

/// Snapshot is a snapshot lent for a call (seam 2): its kind, its bounds as full xids, its command, whether it was
/// taken in recovery, whether its subtransactions overflowed, and its running xids and subtransaction xids lent from
/// Postgres' own arrays, so `txn` keeps none. Only a host makes one, from the snapshot its entry got, which it keeps
/// for the doors of a kind's horizon and command map.
pub struct Snapshot<'a> {
    pub kind: SnapshotKind,
    pub xmin: Xid,
    pub xmax: Xid,
    pub command: Command,
    pub recovery: bool,
    pub overflowed: bool,
    pub xip: &'a [u32],
    pub subxip: &'a [u32],
    /// of is the host's own snapshot the value was read from.
    of: &'a dyn Any,
}

impl<'a> Snapshot<'a> {
    /// made is a host's making of a snapshot from its own, for the simulation.
    #[doc(hidden)]
    pub fn made(
        kind: SnapshotKind,
        (xmin, xmax): (Xid, Xid),
        command: Command,
        (recovery, overflowed): (bool, bool),
        (xip, subxip): (&'a [u32], &'a [u32]),
        of: &'a dyn Any,
    ) -> Snapshot<'a> {
        Snapshot { kind, xmin, xmax, command, recovery, overflowed, xip, subxip, of }
    }

    /// of returns the host's own snapshot the value was read from, for that host's doors.
    #[doc(hidden)]
    pub fn of(&self) -> &'a dyn Any {
        self.of
    }
}

/// Row is a row version's tuple id, 48 bits, which Postgres shows as its `ctid`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Row(pub u64);

/// WalKind is a kind of the fork's WAL records, the one registry of them (design items 8 and 11): each kind is
/// appended only by the crate its doc names; a value up to 15 is the fork's first resource manager's kind, in the four
/// bits Postgres gives a resource manager in a record's info, and a value from 16 is its second's, 15 less.
#[derive(Clone, Copy)]
pub enum WalKind {
    /// RepositoryCreated is a database's repository made by `CREATE DATABASE`, appended only by `store`.
    RepositoryCreated,
    /// RepositoryDropped is a database's repository dropped by `DROP DATABASE`, appended only by `store`.
    RepositoryDropped,
    /// RepositoryCopied is a moving database's repository copied into its new tablespace, appended only by `store`.
    RepositoryCopied,
    /// RepositoryMoved is a moving database's copy made its repository, appended only by `store`.
    RepositoryMoved,
    /// RowChange is a storage's row change, a version and what it takes of the free space map and the tuple id table,
    /// appended only by `txn`'s writes.
    RowChange,
    /// PageLeft is a storage's leave of its fresh page for the next, appended only by `txn`'s inserts.
    PageLeft,
    /// Chunk is a blob's chunk a logged storage's write put into its repository's working state, appended only by
    /// `txn`'s writes before the row that names it.
    Chunk,
    /// BaseAdvanced is a fold's naming of the base it installed in a storage's state, appended only by `txn`'s fold.
    BaseAdvanced,
    /// StorageEmptied is a storage emptied in place by a truncate of a relfilenode its subtransaction made, appended
    /// only by `txn`'s emptying.
    StorageEmptied,
    /// Confirm is a speculative insert's confirm, heap's `XLOG_HEAP_CONFIRM`, appended only by `txn`'s speculative
    /// inserts.
    Confirm,
    /// TidsFreed is the tuple ids VACUUM freed in a storage, with their conflict horizon, appended only by `txn`'s
    /// VACUUM.
    TidsFreed,
    /// MapCarried carries one block's free space map versions into a moved table's new storage (the rewrite text's
    /// section 5).
    MapCarried,
    /// DatabaseChange is a change of a key of one of a database's trees, appended only by `txn`'s writes of them.
    DatabaseChange,
    /// CarryUnlisted is a carry's spill files its primary took back out of its new storage's manifest, appended only
    /// by `txn`'s carry.
    CarryUnlisted,
    /// FoldStatuses is a fold's aborted and killed writers at its point, which the `BaseAdvanced` after it counts,
    /// appended only by `txn`'s fold (bugs row 432).
    FoldStatuses,
}

impl WalKind {
    /// of returns the kind with a value, or nothing for a value no kind has.
    pub fn of(value: u8) -> Option<WalKind> {
        match value {
            4 => Some(WalKind::RepositoryCreated),
            5 => Some(WalKind::RepositoryDropped),
            6 => Some(WalKind::RepositoryCopied),
            7 => Some(WalKind::RepositoryMoved),
            1 => Some(WalKind::RowChange),
            2 => Some(WalKind::PageLeft),
            3 => Some(WalKind::Chunk),
            8 => Some(WalKind::BaseAdvanced),
            9 => Some(WalKind::StorageEmptied),
            10 => Some(WalKind::Confirm),
            11 => Some(WalKind::TidsFreed),
            12 => Some(WalKind::MapCarried),
            13 => Some(WalKind::DatabaseChange),
            14 => Some(WalKind::CarryUnlisted),
            16 => Some(WalKind::FoldStatuses),
            _ => None,
        }
    }

    /// value returns the kind's value.
    pub fn value(self) -> u8 {
        match self {
            WalKind::RepositoryCreated => 4,
            WalKind::RepositoryDropped => 5,
            WalKind::RepositoryCopied => 6,
            WalKind::RepositoryMoved => 7,
            WalKind::RowChange => 1,
            WalKind::PageLeft => 2,
            WalKind::Chunk => 3,
            WalKind::BaseAdvanced => 8,
            WalKind::StorageEmptied => 9,
            WalKind::Confirm => 10,
            WalKind::TidsFreed => 11,
            WalKind::MapCarried => 12,
            WalKind::DatabaseChange => 13,
            WalKind::CarryUnlisted => 14,
            WalKind::FoldStatuses => 16,
        }
    }

    /// name returns the kind's name, which `pg_walinspect` shows as the record's type.
    pub fn name(self) -> &'static CStr {
        match self {
            WalKind::RepositoryCreated => c"RepositoryCreated",
            WalKind::RepositoryDropped => c"RepositoryDropped",
            WalKind::RepositoryCopied => c"RepositoryCopied",
            WalKind::RepositoryMoved => c"RepositoryMoved",
            WalKind::RowChange => c"RowChange",
            WalKind::PageLeft => c"PageLeft",
            WalKind::Chunk => c"Chunk",
            WalKind::BaseAdvanced => c"BaseAdvanced",
            WalKind::StorageEmptied => c"StorageEmptied",
            WalKind::Confirm => c"Confirm",
            WalKind::TidsFreed => c"TidsFreed",
            WalKind::MapCarried => c"MapCarried",
            WalKind::DatabaseChange => c"DatabaseChange",
            WalKind::CarryUnlisted => c"CarryUnlisted",
            WalKind::FoldStatuses => c"FoldStatuses",
        }
    }
}

/// DelayCheckpoint is a window holding off the start of a checkpoint, as Postgres' `DELAY_CHKPT_START` does, in a
/// critical section, so a checkpoint's redo point never falls between a record and the change it describes (design
/// item 8's host rule 1). It ends through `finish` once the change is visible; dropped unfinished, as an error leaving
/// the window drops it, it stops the server, as an error in Postgres' own windows does, and recovery replays the
/// record. Windows don't nest, and no Postgres call that takes `DELAY_CHKPT_START` runs inside one.
pub struct DelayCheckpoint {
    /// host is the host that holds off checkpoints, which lets them start again.
    host: &'static dyn Host,
    /// finished is whether the window's change is visible.
    finished: bool,
}

impl DelayCheckpoint {
    /// finish ends the window once its change is visible.
    pub fn finish(mut self) {
        self.finished = true;
    }
}

impl Drop for DelayCheckpoint {
    fn drop(&mut self) {
        self.host.resume_checkpoints(self.finished);
    }
}

/// CompressDictionary is a zstd dictionary a host prepared for compressing, in the host's own form.
pub struct CompressDictionary(Box<dyn Any + Send + Sync>);

/// DecompressDictionary is a zstd dictionary a host prepared for decompressing, in the host's own form.
pub struct DecompressDictionary(Box<dyn Any + Send + Sync>);

impl CompressDictionary {
    /// new wraps a dictionary a host prepared.
    pub fn new(prepared: Box<dyn Any + Send + Sync>) -> CompressDictionary {
        CompressDictionary(prepared)
    }

    /// prepared returns the dictionary in the form its host prepared it.
    pub fn prepared(&self) -> &(dyn Any + Send + Sync) {
        &*self.0
    }
}

impl DecompressDictionary {
    /// new wraps a dictionary a host prepared.
    pub fn new(prepared: Box<dyn Any + Send + Sync>) -> DecompressDictionary {
        DecompressDictionary(prepared)
    }

    /// prepared returns the dictionary in the form its host prepared it.
    pub fn prepared(&self) -> &(dyn Any + Send + Sync) {
        &*self.0
    }
}

/// ReadHost is the seams a process's host gives the storage crates' reads.
pub trait ReadHost: Sync {
    /// open_to_read opens an existing file to read, returning the host's number for it, which the file seams take.
    fn open_to_read(&self, raise: &MayRaise, path: &Path) -> Result<i32, Error>;

    /// open_if_there opens a file to read, returning the host's number for it, or nothing when no file has the path,
    /// which it answers without an error.
    fn open_if_there(&self, raise: &MayRaise, path: &Path) -> Result<Option<i32>, Error>;

    /// read_at fills a buffer from a file at an offset, or fails on a short read.
    fn read_at(&self, raise: &MayRaise, file: i32, offset: u64, buffer: &mut [u8]) -> Result<(), Error>;

    /// size returns a file's size.
    fn size(&self, raise: &MayRaise, file: i32) -> Result<u64, Error>;

    /// prefetch tells the operating system that bytes of a file will be read soon, where it takes such advice; it
    /// never fails.
    fn prefetch(&self, file: i32, offset: u64, len: u64);

    /// close closes a file; it never fails.
    fn close(&self, file: i32);

    /// tablespace_root returns the directory that holds a tablespace's versioned files, by the tablespace's id: the
    /// data directory for `pg_default`, and the tablespace's version directory for any other. It neither makes the
    /// directory nor looks for it.
    fn tablespace_root(&self, tablespace: u32) -> PathBuf;

    /// list_dir returns the names in a directory in byte order, without `.` and `..`.
    fn list_dir(&self, raise: &MayRaise, path: &Path) -> Result<Vec<String>, Error>;

    /// list_dir_if_there is `list_dir`, or nothing when no directory has the path, which it answers without an error.
    fn list_dir_if_there(&self, raise: &MayRaise, path: &Path) -> Result<Option<Vec<String>>, Error>;

    /// prepare_decompressing prepares a zstd dictionary for decompressing, failing with Postgres' out of memory error
    /// when it can't allocate it.
    fn prepare_decompressing(&self, bytes: &[u8]) -> Result<DecompressDictionary, Error>;

    /// frame_size returns the length a zstd frame records of its bytes, or None when it records none or is no frame.
    fn frame_size(&self, frame: &[u8]) -> Option<u64>;

    /// decompress returns a zstd frame's `size` bytes, decompressed with a dictionary if it names one, or None when
    /// the frame is damaged or holds another size, failing with Postgres' out of memory error when it can't allocate
    /// its memory.
    fn decompress(
        &self,
        frame: &[u8],
        size: usize,
        dictionary: Option<&DecompressDictionary>,
    ) -> Result<Option<Vec<u8>>, Error>;
}

/// Host is the seams a process's host gives the storage crates beyond their reads, which only the server's host and
/// the simulation's give.
pub trait Host: ReadHost {
    /// now returns the time of day.
    fn now(&self) -> SystemTime;

    /// instant returns a reading of the monotonic clock.
    fn instant(&self) -> Instant;

    /// process_id returns the process's id.
    fn process_id(&self) -> u32;

    /// work_mem returns the bytes a query operation may hold before it spills, Postgres' `work_mem` (bugs row 487).
    fn work_mem(&self) -> u64;

    /// session_database returns the database the session is connected to, by its id and the tablespace of its own
    /// directory, or none in a process connected to no database.
    fn session_database(&self) -> Option<(NonZeroU32, u32)>;

    /// exiting returns whether the process's exit has begun.
    fn exiting(&self) -> bool;

    /// unique returns a number no other call of it in the process returns.
    fn unique(&self) -> u64;

    /// check_interrupts fails with Postgres' error once an interrupt is pending, such as a cancel of the statement.
    fn check_interrupts(&self, raise: &MayRaise) -> Result<(), Error>;

    /// hold_interrupts holds the process's interrupts until a resume, as Postgres' HOLD_INTERRUPTS does.
    fn hold_interrupts(&self);

    /// resume_interrupts gives back a hold of the process's interrupts, as Postgres' RESUME_INTERRUPTS does.
    fn resume_interrupts(&self);

    /// open opens a file to write, or creates one, returning the host's number for it, which the file seams take.
    fn open(&self, raise: &MayRaise, path: &Path, how: Open) -> Result<i32, Error>;

    /// create_if_absent creates a file and opens it to read and write, returning the host's number for it, or nothing
    /// when a file already has the path, which it answers without an error.
    fn create_if_absent(&self, raise: &MayRaise, path: &Path) -> Result<Option<i32>, Error>;

    /// open_scratch opens a temporary file of the transaction's, returning the host's number for it, a file its close,
    /// its resource owner's release, the process's exit, and a restart each delete.
    fn open_scratch(&self, raise: &MayRaise) -> Result<i32, Error>;

    /// write_at writes bytes to a file at an offset.
    fn write_at(&self, raise: &MayRaise, file: i32, offset: u64, bytes: &[u8]) -> Result<(), Error>;

    /// truncate cuts a file to a length.
    fn truncate(&self, raise: &MayRaise, file: i32, len: u64) -> Result<(), Error>;

    /// sync makes a file's writes durable, as Postgres' own fsync setting does for its files.
    fn sync(&self, raise: &MayRaise, file: i32) -> Result<(), Error>;

    /// rename renames a file, durably once its directory's sync is.
    fn rename(&self, raise: &MayRaise, from: &Path, to: &Path) -> Result<(), Error>;

    /// rename_durably renames a file, durably once it returns.
    fn rename_durably(&self, raise: &MayRaise, from: &Path, to: &Path) -> Result<(), Error>;

    /// remove_durably removes a file, durably once it returns.
    fn remove_durably(&self, raise: &MayRaise, path: &Path) -> Result<(), Error>;

    /// remove_durably_if_there removes a file and reports whether one was there, which it answers without an error,
    /// with the directory's entries durable once it returns either way.
    fn remove_durably_if_there(&self, raise: &MayRaise, path: &Path) -> Result<bool, Error>;

    /// remove_leftover removes a file a failure left behind, if it is there; it never fails, reporting a removal that
    /// didn't happen in the server's log, and makes nothing durable.
    fn remove_leftover(&self, path: &Path);

    /// rename_back takes a rename back durably after a failure and reports whether it did; it never fails, reporting
    /// a rename that didn't happen in the server's log.
    fn rename_back(&self, from: &Path, to: &Path) -> bool;

    /// remove_tree removes a directory and everything in it and reports whether all of it is gone, a directory that
    /// was never there included; what it couldn't remove it reports in the server's log, and it makes nothing durable.
    fn remove_tree(&self, raise: &MayRaise, path: &Path) -> Result<bool, Error>;

    /// sync_path makes a file's or directory's contents durable.
    fn sync_path(&self, raise: &MayRaise, path: &Path, directory: bool) -> Result<(), Error>;

    /// make_dir makes a directory, if it doesn't exist.
    fn make_dir(&self, raise: &MayRaise, path: &Path) -> Result<(), Error>;

    /// disk_space returns the bytes still free to the server on a directory's volume, and the bytes it holds.
    fn disk_space(&self, raise: &MayRaise, path: &Path) -> Result<(u64, u64), Error>;

    /// tree_bytes returns the bytes of the files under a path, through its subdirectories, 0 when nothing is there.
    fn tree_bytes(&self, raise: &MayRaise, path: &Path) -> Result<u64, Error>;

    /// nap waits on the process's latch for a while, ending early when it is set and raising a pending cancel.
    fn nap(&self, raise: &MayRaise, duration: Duration) -> Result<(), Error>;

    /// wal_insert appends a record of a kind to the WAL, returning the position just past it; a record holds at least
    /// one byte.
    fn wal_insert(&self, raise: &MayRaise, kind: WalKind, body: &[u8]) -> Result<Lsn, Error>;

    /// wal_new_cid appends heap's own record of the command ids of a row a transaction wrote or changed in a relation
    /// logical decoding reads, Postgres' `XLOG_HEAP2_NEW_CID`, returning the position just past it.
    fn wal_new_cid(&self, raise: &MayRaise, storage: &Storage, tid: Row, cid: NewCid) -> Result<Lsn, Error>;

    /// wal_flush makes the WAL durable up to a position.
    fn wal_flush(&self, raise: &MayRaise, lsn: Lsn) -> Result<(), Error>;

    /// fold_point returns the WAL's insert position once every commit whose record lies below it has marked the commit
    /// log, waiting out the backends that delay a checkpoint's start, as a checkpoint's redo point waits (PLAN item 7).
    fn fold_point(&self, raise: &MayRaise) -> Result<Lsn, Error>;

    /// checkpoint_now requests a fast checkpoint, forced though nothing was written since the last, and waits for it.
    fn checkpoint_now(&self, raise: &MayRaise) -> Result<(), Error>;

    /// quiet_database takes a database's lock and ends its sessions, on a hot standby only, as the redo of Postgres'
    /// drop of a database does.
    fn quiet_database(&self, raise: &MayRaise, database: u32) -> Result<(), Error>;

    /// unquiet_database releases the lock `quiet_database` took, on a hot standby only.
    fn unquiet_database(&self, raise: &MayRaise, database: u32) -> Result<(), Error>;

    /// last_redo returns the redo point of the last checkpoint or restartpoint completed, as the control file holds
    /// it.
    fn last_redo(&self, raise: &MayRaise) -> Result<Lsn, Error>;

    /// wal_stand returns where the server's WAL stands.
    fn wal_stand(&self) -> WalStand;

    /// warn_kept_wal reads the WAL from one position to another, on a timeline and up to a position, and reports at
    /// WARNING, naming the segment, where it can't be read, for the WAL kept for a manifest's fallback.
    fn warn_kept_wal(&self, raise: &MayRaise, from: Lsn, to: Lsn, timeline: u32, upto: Lsn) -> Result<(), Error>;

    /// next_fork_record reads the first of the fork's records whose kind is in a mask, bit `n` for kind `n`, past a
    /// record's end and up to the replayed WAL in recovery, the flushed WAL otherwise, as its kind, end, and data.
    fn next_fork_record(&self, raise: &MayRaise, from: Lsn, kinds: u32) -> Result<Option<ForkRecord>, Error>;

    /// warn reports an error at WARNING with a context before its message, for work that goes on past it.
    fn warn(&self, raise: &MayRaise, error: Error, context: &CStr) -> Result<(), Error>;

    /// log reports a message in the server's log at LOG.
    fn log(&self, raise: &MayRaise, message: &CStr) -> Result<(), Error>;

    /// refuse_recovery publishes a standby's root mismatch, with the detail of the FATAL that ends the startup process,
    /// and wakes the startup process, which raises it.
    fn refuse_recovery(&self, detail: &str);

    /// delay_checkpoints opens a checkpoint delay in a critical section, refusing one outside a transaction, which a
    /// checkpoint wouldn't wait for, or inside another delay; it first loads each pause the window holds, which
    /// allocates, so that the pauses inside run with none.
    fn delay_checkpoints(&self, raise: &MayRaise, pauses: &[&'static CStr]) -> Result<(), Error>;

    /// resume_checkpoints ends a checkpoint delay, stopping the server when it ends unfinished or its flag was cleared
    /// inside it; it never returns an error.
    fn resume_checkpoints(&self, finished: bool);

    /// xid_status says whether a transaction or a subtransaction is in progress, committed, or aborted, by Postgres'
    /// own order of checks, the running transactions before the commit log, checking no range of the xid.
    fn xid_status(&self, raise: &MayRaise, xid: Xid) -> Result<Status, Error>;

    /// xid_in_progress says whether a transaction or a subtransaction is in progress, by the running transactions,
    /// for an xid the commit log may no longer keep; an xid 2^31 or more below the next one is in progress to no one.
    fn xid_in_progress(&self, raise: &MayRaise, xid: Xid) -> Result<bool, Error>;

    /// xids_answered says whether `xid_in_progress` answers for every running and prepared transaction.
    fn xids_answered(&self) -> bool;

    /// removable_in says whether a NonVacuumable snapshot's horizon has passed an xid, so that a version it deleted is
    /// dead to everyone: Postgres' `GlobalVisTestIsRemovableFullXid` over the snapshot's own horizon, which it may move.
    fn removable_in(&self, raise: &MayRaise, snapshot: &dyn Any, xid: Xid) -> Result<bool, Error>;

    /// current_writer returns the current subtransaction's xid and its top-level transaction's, assigning each that
    /// has none, the top-level one first; it raises where none can be assigned: in recovery, in a parallel worker,
    /// and near wraparound.
    fn current_writer(&self, raise: &MayRaise) -> Result<(Xid, Xid), Error>;

    /// current_command returns the current command's id, marking it used when `used` says the command writes, which
    /// a parallel worker can't.
    fn current_command(&self, raise: &MayRaise, used: bool) -> Result<Command, Error>;

    /// xid_is_current reports whether an xid is the current transaction's or one of its subtransactions' that hasn't
    /// aborted.
    fn xid_is_current(&self, xid: Xid) -> bool;

    /// combo_command returns the combo command id a change at `cmax` gives a row its own transaction's `writer` wrote
    /// at `cmin`, as Postgres' `HeapTupleHeaderAdjustCmax` takes one from the backend's combo table.
    fn combo_command(&self, raise: &MayRaise, writer: Xid, cmin: Command, cmax: Command) -> Result<Command, Error>;

    /// key_lock takes the key lock's stripes exclusively, in the order given, which is their index order ((4b)'s key
    /// lock); a stripe is held only between its take and its release, never across a wait.
    fn key_lock(&self, raise: &MayRaise, stripes: &[u32]) -> Result<(), Error>;

    /// key_unlock releases stripes the process holds, moving each one's push count first when it pushed under them; it
    /// never fails.
    fn key_unlock(&self, stripes: &[u32], pushed: bool);

    /// key_count reads a stripe's push count, which every release of the stripe after a push under it moves.
    fn key_count(&self, stripe: u32) -> u64;

    /// key_hold sets a stripe's hold-back flag for the running process unless another process's is set, and reports
    /// whether the running process's is ((4b)'s §3).
    fn key_hold(&self, stripe: u32) -> bool;

    /// key_release clears a stripe's hold-back flag if the running process set it, and wakes the writers asleep on it;
    /// it never fails.
    fn key_release(&self, stripe: u32);

    /// key_held_wait sleeps while another process's hold-back flag of a stripe is set, ending at a cancel.
    fn key_held_wait(&self, raise: &MayRaise, stripe: u32) -> Result<(), Error>;

    /// xact_wait waits until a transaction ends, as Postgres' `XactLockTableWait` does with the error context of a
    /// row's operation when one is named, or with `wait` false answers at once whether it has ended
    /// (`ConditionalXactLockTableWait`); it reports whether the transaction ended.
    fn xact_wait(&self, raise: &MayRaise, xid: Xid, on: Option<Operation>, wait: bool) -> Result<bool, Error>;

    /// speculative_wait waits until a running speculative insert completes, Postgres' `SpeculativeInsertionWait` on its
    /// transaction's speculative insertion lock of its token.
    fn speculative_wait(&self, raise: &MayRaise, xid: Xid, token: u32) -> Result<(), Error>;

    /// lock_tuple takes a row's tuple lock in heap's heavyweight mode for a row lock mode, as `heap_acquire_tuplock`
    /// does, waiting while another holds a conflicting one, or with `wait` false answering at once whether it took it.
    fn lock_tuple(&self, raise: &MayRaise, tuple: TupleLock, wait: bool) -> Result<bool, Error>;

    /// unlock_tuple releases a row's tuple lock the process holds; it never fails.
    fn unlock_tuple(&self, tuple: TupleLock);

    /// actor returns the simulation's actor that runs, a backend whose backend-local state is its own, or None for the
    /// process's one backend, as every server process is.
    fn actor(&self) -> Option<u64>;

    /// yield_point lets the simulation hand the run to another actor before a backend-local lock's take; the server's
    /// processes run on their own, so it does nothing there.
    fn yield_point(&self);

    /// pause is where the fork's tests hold the calling process at a place a crate named: the injection point of
    /// that name in the server, run from the backend's cache with no allocation inside a checkpoint window, whose open
    /// loaded it, and a yield point that carries the name in the simulation, which fails the run for a pause inside a
    /// window whose open didn't load its name.
    fn pause(&self, raise: &MayRaise, point: &CStr) -> Result<(), Error>;

    /// injected says whether the fork's tests attached the server's injection point of a name, where a crate takes a
    /// test's path; the simulation attaches none.
    fn injected(&self, raise: &MayRaise, point: &CStr) -> Result<bool, Error>;

    /// at_exit lists an exit work of the calling process by the type that names it, once however often it is asked,
    /// and refuses one asked once the process's exit work has begun.
    fn at_exit(&self, work: TypeId, run: fn(&mut Io) -> Result<(), Error>) -> Result<(), Error>;

    /// prepare_compressing prepares a zstd dictionary for compressing at a level, failing with Postgres' out of memory
    /// error when it can't allocate it.
    fn prepare_compressing(&self, bytes: &[u8], level: i32) -> Result<CompressDictionary, Error>;

    /// compress returns bytes compressed as one zstd frame that records their length, at a level or with a
    /// dictionary, failing with Postgres' out of memory error when it can't allocate its memory.
    fn compress(&self, bytes: &[u8], level: i32, dictionary: Option<&CompressDictionary>) -> Result<Vec<u8>, Error>;

    /// train returns a zstd dictionary of at most `capacity` bytes trained on samples, or None when it can't train
    /// one on them.
    fn train(&self, samples: &[&[u8]], capacity: usize) -> Option<Vec<u8>>;

    /// shared_make takes a free slot of the pool of a shape for a record its maker owns, its atomics zero and its blobs
    /// empty, returning its place, or nothing when a sweep of the pool finds no free slot.
    fn shared_make(&self, raise: &MayRaise, kind: &shared::Kind) -> Result<Option<u64>, Error>;

    /// shared_publish publishes a record the process owns, giving its ownership up, and returns its location; it never
    /// fails.
    fn shared_publish(&self, place: u64) -> u64;

    /// shared_claim takes the ownership of the published record of a shape at a location, returning its place, or
    /// nothing for a location that is stale or a record that isn't published, and refusing a location outside the
    /// shape's pool.
    fn shared_claim(&self, kind: &shared::Kind, location: u64) -> Result<Option<u64>, Error>;

    /// shared_free frees a record the process owns, at once when no process holds it and otherwise at its last close;
    /// it never fails.
    fn shared_free(&self, place: u64);

    /// shared_give_up gives up a record the process still owns, as its dropped owner does: one never published is
    /// freed, and one claimed is published again; it never fails.
    fn shared_give_up(&self, place: u64);

    /// shared_open opens the published record of a shape at a location, returning its place, or nothing for a
    /// location that is stale or a record that isn't published, and refusing a location outside the shape's pool.
    fn shared_open(&self, raise: &MayRaise, kind: &shared::Kind, location: u64) -> Result<Option<u64>, Error>;

    /// shared_close closes a record the process opened; it never fails.
    fn shared_close(&self, place: u64);

    /// shared_reclaim looks at the published record of a shape at a location for a reclaim, taking it when no process
    /// holds it and no other reclaim is looking at it, and refusing a location outside the shape's pool.
    fn shared_reclaim(&self, kind: &shared::Kind, location: u64) -> Result<shared::Looked, Error>;

    /// shared_reclaimed frees a record the process's reclaim took if no process opened it since, and otherwise gives
    /// it up, reporting whether it freed it; it never fails.
    fn shared_reclaimed(&self, place: u64) -> bool;

    /// shared_unreclaim gives up a record the process's reclaim still has; it never fails.
    fn shared_unreclaim(&self, place: u64);

    /// shared_sweep lists the published records of the pool of a shape: each one's location and the value of one of
    /// its atomics, read with no handle, for the records published at one generation both before and after the read.
    /// The fork's tests pause it between a slot's first load and the read.
    fn shared_sweep(&self, raise: &MayRaise, kind: &shared::Kind, atomic: usize) -> Result<Vec<(u64, u64)>, Error>;

    /// shared_single returns the place of the server's one record of a shape.
    fn shared_single(&self, kind: &shared::Kind) -> Result<u64, Error>;

    /// shared_ready reports whether the server made the fork's shared memory for this process, which a bootstrap
    /// process, running no preload window, has none of.
    fn shared_ready(&self) -> bool;

    /// shared_lock takes a record's lock, refusing one the process holds.
    fn shared_lock(&self, raise: &MayRaise, place: u64, lock: usize, exclusive: bool) -> Result<(), Error>;

    /// shared_unlock releases a record's lock the process holds; it never fails.
    fn shared_unlock(&self, place: u64, lock: usize);

    /// flag_take sets a record's flag, waiting interruptibly while another process holds it, and refusing one the
    /// process holds.
    fn flag_take(&self, raise: &MayRaise, place: u64, flag: usize) -> Result<(), Error>;

    /// flag_try sets a record's flag when no other process holds it, answering whether it did, and refuses one the
    /// process holds.
    fn flag_try(&self, raise: &MayRaise, place: u64, flag: usize) -> Result<bool, Error>;

    /// flag_clear clears a record's flag the process holds and wakes its waiters; it never fails.
    fn flag_clear(&self, place: u64, flag: usize);

    /// atomic operates on a record's atomic, returning its value before.
    fn atomic(&self, place: u64, atomic: usize, op: shared::Op) -> u64;

    /// shared_atomics returns the count of atomics the record at a place was laid out with, its run's words
    /// included.
    fn shared_atomics(&self, place: u64) -> usize;

    /// blob_read returns the bytes of a record's blob, under the blob's lock.
    fn blob_read(&self, place: u64, blob: usize) -> Vec<u8>;

    /// blob_write writes the bytes of a record's blob in place, under the blob's lock held exclusively, refusing bytes
    /// longer than the blob's largest size with nothing written.
    fn blob_write(&self, place: u64, blob: usize, bytes: &[u8]) -> Result<(), Error>;

    /// entry_read returns the bytes of an entry of a record's array, under the array's lock, refusing an entry past
    /// the array's count.
    fn entry_read(&self, place: u64, array: usize, entry: usize) -> Result<Vec<u8>, Error>;

    /// entry_write writes the bytes of an entry of a record's array in place, under the array's lock held exclusively,
    /// refusing an entry past the array's count and bytes of another length than an entry's with nothing written.
    fn entry_write(&self, place: u64, array: usize, entry: usize, bytes: &[u8]) -> Result<(), Error>;

    /// shared_location returns the location of the record at a place: a pooled record's, which the process has a
    /// handle of, or zero for a single record, which is the one record of its shape.
    fn shared_location(&self, place: u64) -> u64;

    /// shared_live reports whether the pooled record at a location still has its slot.
    fn shared_live(&self, location: u64) -> bool;

    /// shared_gone reports whether the pooled record of a shape that a location named no longer has its slot,
    /// refusing a location outside the shape's pool.
    fn shared_gone(&self, kind: &shared::Kind, location: u64) -> Result<bool, Error>;

    /// slots_name loads the name of a record's slots array.
    fn slots_name(&self, place: u64, array: usize) -> u64;

    /// slots_rename stores a name in a record's slots array if the array still holds the name its caller loaded,
    /// reporting whether it did; it never fails.
    fn slots_rename(&self, place: u64, array: usize, loaded: u64, name: u64) -> bool;

    /// slots_empty zeroes the count of a record's slots array, under the barrier of the record it names, held
    /// exclusively; it never fails.
    fn slots_empty(&self, place: u64, array: usize);

    /// slots_claim adds an entry to a record's slots array and writes its bytes, under the barrier of the record the
    /// array names, held shared, reporting whether it did: an array whose every entry is claimed takes none. The
    /// fork's tests pause it between its add and its write.
    fn slots_claim(&self, raise: &MayRaise, place: u64, array: usize, bytes: &[u8]) -> Result<bool, Error>;

    /// slots_read returns the claimed entries of a record's slots array, under the barrier of the record it names,
    /// held exclusively.
    fn slots_read(&self, place: u64, array: usize) -> Vec<Vec<u8>>;

    /// words_processes returns the numbers of the processes whose slot of a kind of words can hold a word.
    fn words_processes(&self, kind: &shared::Kind) -> Result<Vec<u32>, Error>;

    /// words_mine returns the place of the running process's slot of a kind of words.
    fn words_mine(&self, kind: &shared::Kind) -> Result<u64, Error>;

    /// word_store stores a word of the running process's slot at a place; it never fails.
    fn word_store(&self, place: u64, word: usize, value: u64);

    /// word_store_own stores a word of the running process's slot of a kind at a position of the server's table of
    /// kinds; it never fails.
    fn word_store_own(&self, index: usize, kind: &shared::Kind, word: usize, value: u64);

    /// word_load loads a word of a process's slot of a kind of words, refusing a process number past the server's
    /// processes.
    fn word_load(&self, kind: &shared::Kind, proc: u32, word: usize) -> Result<u64, Error>;

    /// words_place returns the place of a process's slot of a kind of words, which the host's atomics take, refusing a
    /// process with no slot.
    fn words_place(&self, kind: &shared::Kind, proc: u32) -> Result<u64, Error>;

    /// epochs_lock takes the fork-wide epoch lock exclusively.
    fn epochs_lock(&self, raise: &MayRaise) -> Result<(), Error>;

    /// epochs_unlock releases the epoch lock; it never fails.
    fn epochs_unlock(&self);

    /// epochs_advance returns the current epoch and advances it, under the epoch lock.
    fn epochs_advance(&self) -> u64;

    /// epochs_oldest returns the oldest epoch a live reader started at, or the current one, under the epoch lock.
    fn epochs_oldest(&self) -> u64;

    /// epochs_read starts a reader at the current epoch, published in the process's slot, returning the epoch.
    fn epochs_read(&self, raise: &MayRaise) -> Result<u64, Error>;

    /// epochs_end ends a reader of the process; it never fails.
    fn epochs_end(&self, epoch: u64);

    /// proc_number returns the process's number among the server's processes.
    fn proc_number(&self) -> u32;

    /// delta_hold begins a hold of the running process on the delta's pages and returns its epoch.
    fn delta_hold(&self, raise: &MayRaise) -> Result<u64, Error>;

    /// delta_release ends a hold of the running process; it never fails.
    fn delta_release(&self, epoch: u64);

    /// delta_open reaches the page a name names under a hold begun at an epoch, refusing a name that names no page
    /// the hold reaches.
    fn delta_open(&self, name: delta::Page, epoch: u64) -> Result<delta::directory::Opened, Error>;

    /// delta_bytes lends a range of an opened page's bytes, refusing one that ends past the page's filled mark.
    fn delta_bytes(&self, opened: &delta::directory::Opened, offset: u64, len: u64) -> Result<cache::Bytes, Error>;

    /// delta_room returns how many bytes an opened page takes above its filled mark; it never fails.
    fn delta_room(&self, opened: &delta::directory::Opened) -> u64;

    /// delta_filled returns how many bytes an opened page holds below its filled mark; it never fails.
    fn delta_filled(&self, opened: &delta::directory::Opened) -> u64;

    /// delta_word operates on a word of a page's body, returning its value before; it never fails.
    fn delta_word(&self, body: u64, index: u64, op: delta::WordOp) -> u64;

    /// delta_alloc takes a page of each size, all or none, listing their names in room its caller reserved; it
    /// answers nothing when it took them and the count of frees it loaded when the delta refused them.
    fn delta_alloc(
        &self,
        raise: &MayRaise,
        sizes: &[delta::Size],
        names: &mut Vec<delta::Page>,
    ) -> Result<Option<u64>, Error>;

    /// delta_alloc_replacing takes a page of a size for the copy of a page it replaces, past the budget while no other
    /// replacement is, listing its name in room its caller reserved; it answers as `delta_alloc` does.
    fn delta_alloc_replacing(
        &self,
        raise: &MayRaise,
        size: delta::Size,
        replaced: delta::Page,
        names: &mut Vec<delta::Page>,
    ) -> Result<Option<u64>, Error>;

    /// delta_give_back gives back a page the running process owns, and does nothing for one it linked; it never
    /// fails.
    fn delta_give_back(&self, name: delta::Page);

    /// delta_owned_append appends a record to a page the running process owns, returning its offset.
    fn delta_owned_append(&self, name: delta::Page, record: &[u8]) -> Result<u32, Error>;

    /// delta_owned_store stores a word of a page the running process owns.
    fn delta_owned_store(&self, name: delta::Page, index: u64, value: u64) -> Result<(), Error>;

    /// delta_link records the lock that may append to a page the running process owns and makes the page live; it
    /// never fails.
    fn delta_link(&self, name: delta::Page, owner: delta::directory::Owner);

    /// delta_append appends a record to an opened page under the lock its slot records, returning its offset.
    fn delta_append(
        &self,
        opened: &delta::directory::Opened,
        owner: delta::directory::Owner,
        record: &[u8],
    ) -> Result<u32, Error>;

    /// delta_seal clears an opened page's owner under the lock its slot records.
    fn delta_seal(&self, opened: &delta::directory::Opened, owner: delta::directory::Owner) -> Result<(), Error>;

    /// delta_retire retires one batch of records and names under one take of the epoch lock; it never fails.
    fn delta_retire(&self, records: &[delta::Leaving], names: &[delta::Page]);

    /// delta_make claims a free record of an aged kind for the running process, its parts cleared, and returns its
    /// place and its location, or the count of frees it loaded before it looked when none was free; the name names
    /// the kind's records in the error of a pool of spent slots.
    fn delta_make(&self, raise: &MayRaise, kind: &shared::Kind, name: &str) -> Result<Result<(u64, u64), u64>, Error>;

    /// delta_record returns the place of the record of an aged kind a location names under a hold begun at an epoch,
    /// refusing one the hold doesn't reach.
    fn delta_record(&self, kind: &shared::Kind, location: u64, epoch: u64) -> Result<u64, Error>;

    /// delta_publish publishes a record the running process made, off its list.
    fn delta_publish(&self, place: u64);

    /// delta_unmake gives back a record the running process made and didn't link, and does nothing for one it
    /// linked.
    fn delta_unmake(&self, place: u64);

    /// delta_pass runs the free pass; it never fails.
    fn delta_pass(&self);

    /// delta_advanced advances the epoch and returns the epoch it passed and the oldest one a live reader or hold
    /// started at; it never fails.
    fn delta_advanced(&self) -> (u64, u64);

    /// delta_used returns the bytes of the delta in use.
    fn delta_used(&self) -> u64;

    /// delta_budget returns the delta's memory in bytes, `delta_memory`.
    fn delta_budget(&self) -> u64;

    /// delta_seed returns the random seed of the delta's shapes in memory.
    fn delta_seed(&self) -> [u8; 32];

    /// delta_wait waits at the budget while the count of frees is the one given, refusing a process that has a hold
    /// or owns a page.
    fn delta_wait(&self, raise: &MayRaise, frees: u64) -> Result<(), Error>;

    /// backup_begin counts a base backup in and then waits while a collection runs (the collector text's §3.3).
    fn backup_begin(&self, raise: &MayRaise) -> Result<(), Error>;

    /// backup_end counts a base backup out.
    fn backup_end(&self);

    /// backups_running reports whether a base backup is counted in.
    fn backups_running(&self) -> bool;

    /// collection_begin sets the collection's word unless a base backup is counted in, answering whether it may run.
    fn collection_begin(&self) -> bool;

    /// collection_end clears the collection's word and wakes the backups waiting for it.
    fn collection_end(&self);

    /// collection_ask asks the collector worker for a full collection of a database's repository and waits for its
    /// answer, ending at a cancel.
    fn collection_ask(&self, raise: &MayRaise, database: u32) -> Result<[u64; 3], Error>;

    /// collection_asked returns the unanswered ask's word and the database it asks for.
    fn collection_asked(&self) -> Option<(u64, u32)>;

    /// collection_answer answers an ask and wakes its asker.
    fn collection_answer(&self, word: u64, counts: [u64; 3]);

    /// collector_started records the running process as the collector worker, whose latch an ask sets.
    fn collector_started(&self);

    /// database_locked takes a database's lock in session mode without waiting, answering whether it took it.
    fn database_locked(&self, raise: &MayRaise, database: u32) -> Result<bool, Error>;

    /// database_unlocked gives back a database's lock that `database_locked` took.
    fn database_unlocked(&self, database: u32);

    /// database_lock_waited reports whether a session waits for the lock this process holds on a database.
    fn database_lock_waited(&self, raise: &MayRaise, database: u32) -> Result<bool, Error>;

    /// cache_get pins the frame of a node cache that holds a key's node whole, or returns None.
    fn cache_get(
        &self,
        raise: &MayRaise,
        placement: cache::Placement,
        key: &cache::Key,
    ) -> Result<Option<cache::Found>, Error>;

    /// cache_install claims a frame of a node cache for a node of `len` bytes under its key, has `fill` write them,
    /// and pins it, or pins the frame another backend put the key in meanwhile, or returns None when no frame of that
    /// size is free; it calls `fill` only for a frame it returns.
    fn cache_install(
        &self,
        raise: &MayRaise,
        placement: cache::Placement,
        key: &cache::Key,
        len: usize,
        fill: &mut dyn FnMut(&mut [u8]),
    ) -> Result<Option<cache::Found>, Error>;

    /// cache_contains reports whether a node cache's table names a key, pinning nothing and counting as no use.
    fn cache_contains(&self, raise: &MayRaise, placement: cache::Placement, key: &cache::Key) -> Result<bool, Error>;

    /// cache_unpin gives up the process's pin of a frame; it neither allocates nor raises.
    fn cache_unpin(&self, placement: cache::Placement, frame: u32);

    /// cache_make_local fixes the size of the backend's own node cache, once.
    fn cache_make_local(&self, raise: &MayRaise, bytes: usize) -> Result<(), Error>;

    /// owner_enlarge makes room with the current resource owner for one more thing it remembers and names that
    /// owner, or refuses when there is none.
    fn owner_enlarge(&self, raise: &MayRaise) -> Result<u64, Error>;

    /// owner_top_enlarge makes room with the top-level transaction's resource owner for one more owned record and names
    /// that owner, or refuses outside a transaction.
    fn owner_top_enlarge(&self, raise: &MayRaise) -> Result<u64, Error>;

    /// owner_remember has a resource owner remember a pin, in the room `owner_enlarge` made; it never fails.
    fn owner_remember(&self, owner: u64, pin: u64);

    /// owner_forget has a resource owner forget a pin it remembers and hasn't released; it never fails.
    fn owner_forget(&self, owner: u64, pin: u64);

    /// owned_remember has a resource owner remember an owned record, in the room `owner_enlarge` made, with its
    /// kind's release for a host whose own owners release it; it never fails.
    fn owned_remember(&self, owner: u64, record: u64, release: fn(u32));

    /// owned_forget has a resource owner forget an owned record it remembers and hasn't released; it never fails.
    fn owned_forget(&self, owner: u64, record: u64);

    /// btree_compare returns how a btree class's comparison function orders two values, below, at, or above zero; it
    /// runs user code, which `btree_compare`'s door reaches only with no lock of the fork's held (design item 13).
    fn btree_compare(&self, raise: &MayRaise, compared: &Compared, a: &[u8], b: &[u8]) -> Result<i32, Error>;

    /// sort_key returns a text's sort key under a collation, as Postgres' `pg_strnxfrm` writes it, which a comparator
    /// re-encodes a key's long value with; it reads the catalog, which `sort_key`'s door reaches only with no lock of
    /// the fork's held, as `btree_compare`'s does.
    fn sort_key(&self, raise: &MayRaise, collation: u32, text: &[u8]) -> Result<Vec<u8>, Error>;

    /// reformed re-forms a fold's stored row through its transform into its table's layout in the catalog; it reads the
    /// catalog, which `reformed`'s door reaches only with no lock of the fork's held, as `btree_compare`'s does.
    fn reformed(&self, raise: &MayRaise, row: &ReformRow<'_>) -> Result<ReformedRow, Error>;
}

/// ReformRow is a stored row a fold re-forms through `reformed`: its table's OID, the table's key columns as a slot
/// keeps them beside a row, its key, its row stamp's writer and tuple id, its tuple, its values' ids out of line, and
/// the encoded transform from its layout to its chain's end.
pub struct ReformRow<'a> {
    pub relation: u32,
    pub keyed: &'a [u8],
    pub key: &'a [u8],
    pub stamp: (u64, u64),
    pub tuple: &'a [u8],
    pub values: &'a [u32],
    pub transform: &'a [u8],
}

/// ReformedRow is a row re-formed to its chain's end: its tuple, the offsets of the addresses it holds, its values'
/// ids out of line, and its heap bytes.
pub struct ReformedRow {
    pub tuple: Vec<u8>,
    pub addresses: Vec<u32>,
    pub values: Vec<u32>,
    pub heap_bytes: u32,
}

/// Compared is how `btree_compare` calls a comparison function: its OID, the collation it compares under, and its
/// type's stored length and whether the type is passed by value, whose values are its word's bytes, low byte first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Compared {
    pub function: u32,
    pub collation: u32,
    pub len: i16,
    pub by_value: bool,
}

/// HOST is the host installed for the process.
static HOST: OnceLock<&'static dyn Host> = OnceLock::new();

/// READING is the reads of the host installed for the process.
static READING: OnceLock<&'static dyn ReadHost> = OnceLock::new();

/// Refused is the error of installing a host in a process that has one.
#[derive(Debug)]
pub struct Refused;

/// install installs the process's host, refusing to replace the one it has.
pub fn install(host: &'static dyn Host) -> Result<(), Refused> {
    HOST.set(host).map_err(|_| Refused)?;
    READING.set(host).map_err(|_| Refused)
}

/// installed returns whether the process has a host.
pub fn installed() -> bool {
    HOST.get().is_some()
}

/// acting returns whether the running backend is one of the simulation's actors.
fn acting() -> bool {
    HOST.get().is_some_and(|host| host.actor().is_some())
}

/// yield_point passes the host's yield point before a backend-local lock's take, if a host is installed.
fn yield_point() {
    if let Some(host) = HOST.get() {
        host.yield_point();
    }
}

/// Exit is work a process does as it exits, named by the type that implements it.
pub trait Exit: 'static {
    /// run does the work, with the process's words and handles still its own and none of its flags held.
    fn run(io: &mut Io) -> Result<(), Error>;
}

/// at_exit has the calling process run an exit work as it exits, once however often it is registered. A crate
/// registers its work before it takes what the work cleans up, and a registration made once the process's exit work
/// has begun is refused.
pub fn at_exit<E: Exit>() -> Result<(), Error> {
    current().at_exit(TypeId::of::<E>(), E::run)
}

/// current returns the process's host, which every entry installs before a storage crate runs.
fn current() -> &'static dyn Host {
    HOST.get().copied().expect("a host is installed before any storage crate runs")
}

/// reading returns the reads of the process's host.
fn reading() -> &'static dyn ReadHost {
    READING.get().copied().expect("a host is installed before any storage crate runs")
}

/// pause holds the calling process at a named place when the fork's tests ask for it. A crate reaches it only
/// through `pause!`, whose name is a literal.
#[cfg(feature = "testing")]
pub fn pause<P: shared::Pauses>(_: &mut P, point: &'static CStr) -> Result<(), Error> {
    current().pause(&MayRaise::of(), point)
}

/// injected says whether the fork's tests attached a named injection point, where a crate takes a test's path.
#[cfg(feature = "testing")]
pub fn injected(io: &mut impl IoDoors, point: &'static CStr) -> Result<bool, Error> {
    current().injected(&doors(io), point)
}

/// injected is false in a build without the tests' capability: a constant function, which
/// the compiler holds to calling no host.
#[cfg(not(feature = "testing"))]
pub const fn injected<D: IoDoors>(_: &mut D, _: &'static CStr) -> Result<bool, Error> {
    Ok(false)
}

/// pause is nothing in a build without the tests' capability: a constant function, which
/// the compiler holds to calling no host.
#[cfg(not(feature = "testing"))]
pub const fn pause<P: shared::Pauses>(_: &mut P, _: &'static CStr) -> Result<(), Error> {
    Ok(())
}

/// pause marks a place where the fork's tests can hold the calling process: what the caller holds there, and the
/// place's name, which is a literal, so that the text of every pause shows its name. It gives the pause's result,
/// an error when the hold was cancelled.
#[macro_export]
macro_rules! pause {
    ($at:expr, $point:literal) => {{
        #[allow(clippy::disallowed_methods, reason = "the macro is the one way to the pause's door")]
        fn door<P: $crate::shared::Pauses>(
            at: &mut P,
            point: &'static ::core::ffi::CStr,
        ) -> ::core::result::Result<(), $crate::Error> {
            $crate::pause(at, point)
        }
        door($at, $point)
    }};
}

/// now returns the time of day.
pub fn now() -> SystemTime {
    current().now()
}

/// instant returns a reading of the monotonic clock.
pub fn instant() -> Instant {
    current().instant()
}

/// elapsed returns the time since a reading of `instant`, or zero for a reading after now.
pub fn elapsed(since: Instant) -> Duration {
    instant().saturating_duration_since(since)
}

/// process_id returns the process's id.
pub fn process_id() -> u32 {
    current().process_id()
}

/// session_database returns the database the session is connected to, by its id and the tablespace of its own
/// directory; a process connected to no database has none, an internal error, since only a connected backend asks.
pub fn session_database() -> Result<(NonZeroU32, u32), Error> {
    current()
        .session_database()
        .ok_or_else(|| Error::fork(b"XX000", "the process is connected to no database".to_owned()))
}

/// exiting returns whether the process's exit has begun, after which a frame a FATAL left standing never runs again.
pub fn exiting() -> bool {
    current().exiting()
}

/// unique returns a number no other call of it in the process returns.
pub fn unique() -> u64 {
    current().unique()
}

/// open opens a file to write, or creates one.
pub fn open(io: &mut impl IoDoors, path: &Path, how: Open) -> Result<File, Error> {
    let host = current();
    host.open(&doors(io), path, how).map(|number| File { number, host })
}

/// open_to_read opens an existing file to read.
pub fn open_to_read(io: &mut impl ReadDoors, path: &Path) -> Result<File, Error> {
    let host = reading();
    host.open_to_read(&doors(io), path).map(|number| File { number, host })
}

/// open_if_there opens a file to read, or returns nothing when no file has the path: an answer, with no error raised,
/// for a caller that asks whether the file is there.
pub fn open_if_there(io: &mut impl ReadDoors, path: &Path) -> Result<Option<File>, Error> {
    let host = reading();
    Ok(host.open_if_there(&doors(io), path)?.map(|number| File { number, host }))
}

/// create_if_absent creates a file and opens it to read and write, or returns nothing when a file already has the
/// path: an answer, with no error raised, for a caller that asks whether the file is already there.
pub fn create_if_absent(io: &mut impl IoDoors, path: &Path) -> Result<Option<File>, Error> {
    let host = current();
    Ok(host.create_if_absent(&doors(io), path)?.map(|number| File { number, host }))
}

/// open_scratch opens a temporary file of the current transaction's, for work that never becomes durable.
pub fn open_scratch<'io>(io: &mut io::Io) -> Result<ScratchFile<'io>, Error> {
    let host = current();
    let number = host.open_scratch(&doors(io))?;
    Ok(ScratchFile { file: File { number, host }, entry: PhantomData })
}

/// read_at fills a buffer from a file at an offset, or fails on a short read.
pub fn read_at(io: &mut impl ReadDoors, file: &File, offset: u64, buffer: &mut [u8]) -> Result<(), Error> {
    reading().read_at(&doors(io), file.number, offset, buffer)
}

/// write_at writes bytes to a file at an offset.
pub fn write_at(io: &mut impl IoDoors, file: &File, offset: u64, bytes: &[u8]) -> Result<(), Error> {
    current().write_at(&doors(io), file.number, offset, bytes)
}

/// size returns a file's size.
pub fn size(io: &mut impl ReadDoors, file: &File) -> Result<u64, Error> {
    reading().size(&doors(io), file.number)
}

/// truncate cuts a file to a length.
pub fn truncate(io: &mut impl IoDoors, file: &File, len: u64) -> Result<(), Error> {
    current().truncate(&doors(io), file.number, len)
}

/// prefetch tells the operating system that bytes of a file will be read soon; it never fails.
pub fn prefetch(file: &File, offset: u64, len: u64) {
    reading().prefetch(file.number, offset, len)
}

/// sync makes a file's writes durable.
pub fn sync(io: &mut impl IoDoors, file: &File) -> Result<(), Error> {
    current().sync(&doors(io), file.number)
}

/// rename renames a file, durably once its directory's sync is.
pub fn rename(io: &mut impl IoDoors, from: &Path, to: &Path) -> Result<(), Error> {
    current().rename(&doors(io), from, to)
}

/// rename_durably renames a file, durably once it returns.
pub fn rename_durably(io: &mut impl IoDoors, from: &Path, to: &Path) -> Result<(), Error> {
    current().rename_durably(&doors(io), from, to)
}

/// remove_durably removes a file, durably once it returns.
pub fn remove_durably(io: &mut impl IoDoors, path: &Path) -> Result<(), Error> {
    current().remove_durably(&doors(io), path)
}

/// remove_durably_if_there removes a file and reports whether one was there, an answer with no error raised, with the
/// directory's entries durable once it returns either way.
pub fn remove_durably_if_there(io: &mut impl IoDoors, path: &Path) -> Result<bool, Error> {
    current().remove_durably_if_there(&doors(io), path)
}

/// remove_leftover removes a file a failure left behind, if it is there, for the code that returns that failure: it
/// never fails, the host reporting a removal that didn't happen in its log, and it makes nothing durable, so it is
/// only for a file whose name startup's cleanup also removes. It takes the capability of file I/O as every file door
/// does, and raises nothing through it.
pub fn remove_leftover(_: &mut impl IoDoors, path: &Path) {
    current().remove_leftover(path)
}

/// VERSIONED is the directory, in the data directory and in each tablespace's version directory, that holds each
/// database's versioned files in a directory named by the database's id.
pub const VERSIONED: &str = "pg_versioned";

/// PG_DEFAULT is the id of Postgres' default tablespace, `pg_default`, whose files are in the data directory itself.
pub const PG_DEFAULT: u32 = 1663;

/// tablespace_dir returns the directory of a database's versioned files in a tablespace, by their ids, the same in
/// every process: `pg_versioned/<database>` in the data directory for `pg_default`, and in the tablespace's version
/// directory for any other. It does no I/O, so a directory that isn't there is found by whoever opens a file in
/// it.
pub fn tablespace_dir(database: u32, tablespace: u32) -> PathBuf {
    versioned_dir(tablespace).join(database.to_string())
}

/// versioned_dir returns the directory of a tablespace that holds each database's versioned files in a directory of
/// its own, `pg_versioned` in the data directory for `pg_default` and in the tablespace's version directory for any
/// other.
pub fn versioned_dir(tablespace: u32) -> PathBuf {
    reading().tablespace_root(tablespace).join(VERSIONED)
}

/// rename_back takes a rename back durably after a failure, for the code that returns that failure, and reports
/// whether it did: it never fails, the host reporting a rename that didn't happen in its log, so it is only for names
/// that read correctly with the rename not taken back, as they would after a crash at that step. It takes the
/// capability of file I/O as every file door does, and raises nothing through it.
#[must_use]
pub fn rename_back(_: &mut impl IoDoors, from: &Path, to: &Path) -> bool {
    current().rename_back(from, to)
}

/// remove_tree removes a directory and everything in it, as Postgres removes a dropped database's, and reports
/// whether all of it is gone: true also for a directory that was never there, and false when something was left,
/// which the host reports in its log with Postgres' wording. A tree another process removes while this one is
/// removing it is answered as something left, and as gone when asked again. It makes nothing durable, so it is
/// only for a directory nothing reads again, and its one error of its own is a path that can't be passed.
pub fn remove_tree(io: &mut impl IoDoors, path: &Path) -> Result<bool, Error> {
    current().remove_tree(&doors(io), path)
}

/// sync_path makes a file's or directory's contents durable.
pub fn sync_path(io: &mut impl IoDoors, path: &Path, directory: bool) -> Result<(), Error> {
    current().sync_path(&doors(io), path, directory)
}

/// directory_of returns the directory a path is in, `.` for a one-part relative path, as Postgres' `fsync_parent_path`
/// takes it.
pub fn directory_of(path: &Path) -> &Path {
    path.parent().filter(|dir| !dir.as_os_str().is_empty()).unwrap_or(Path::new("."))
}

/// make_dir makes a directory, if it doesn't exist.
pub fn make_dir(io: &mut impl IoDoors, path: &Path) -> Result<(), Error> {
    current().make_dir(&doors(io), path)
}

/// list_dir returns the names in a directory in byte order, without `.` and `..`.
pub fn list_dir(io: &mut impl ReadDoors, path: &Path) -> Result<Vec<String>, Error> {
    reading().list_dir(&doors(io), path)
}

/// list_dir_if_there is `list_dir`, or nothing when no directory has the path: an answer, with no error raised, for a
/// caller that asks whether the directory is there.
pub fn list_dir_if_there(io: &mut impl ReadDoors, path: &Path) -> Result<Option<Vec<String>>, Error> {
    reading().list_dir_if_there(&doors(io), path)
}

/// disk_space returns the bytes still free to the server on a directory's volume, and the bytes it holds, which the
/// collector's schedule compares with `collector_min_free_disk` (the collector text's §8.3).
pub fn disk_space(io: &mut impl IoDoors, path: &Path) -> Result<(u64, u64), Error> {
    current().disk_space(&doors(io), path)
}

/// tree_bytes returns the bytes of the files under a path, through its subdirectories, 0 when nothing is there, which
/// a database's and a tablespace's sizes add for their repositories (design item 5).
pub fn tree_bytes(io: &mut impl IoDoors, path: &Path) -> Result<u64, Error> {
    current().tree_bytes(&doors(io), path)
}

/// nap waits on the process's latch for a while, ending early when it is set and raising a pending cancel, as a
/// collection paced by `collector_io_limit` does (the collector text's §8.3).
pub fn nap(io: &mut impl IoDoors, duration: Duration) -> Result<(), Error> {
    current().nap(&doors(io), duration)
}

/// wal_insert appends a record of a kind to the WAL, returning the position just past it; a record holds at least one
/// byte.
pub fn wal_insert(io: &mut impl IoDoors, kind: WalKind, body: &[u8]) -> Result<Lsn, Error> {
    current().wal_insert(&doors(io), kind, body)
}

/// NewCid is what heap's record of a row's command ids holds, as `log_heap_new_cid` fills it: the command that wrote
/// the row, the command that changed it, and the combo command id that stands for both, each none when heap's is
/// `InvalidCommandId`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NewCid {
    pub cmin: Option<Command>,
    pub cmax: Option<Command>,
    pub combo: Option<Command>,
}

/// wal_new_cid appends heap's own record of the command ids of a row a transaction wrote or changed in a relation
/// logical decoding reads, so decoding finds the row's command ids as it finds heap's (PLAN item 7's historic
/// snapshots); it returns the position just past the record.
pub fn wal_new_cid(io: &mut impl IoDoors, storage: &Storage, tid: Row, cid: NewCid) -> Result<Lsn, Error> {
    current().wal_new_cid(&doors(io), storage, tid, cid)
}

/// wal_flush makes the WAL durable up to a position.
pub fn wal_flush(io: &mut impl IoDoors, lsn: Lsn) -> Result<(), Error> {
    current().wal_flush(&doors(io), lsn)
}

/// fold_point returns a fold's point, `m`: the WAL's insert position once every commit whose record lies below it has
/// marked the commit log and its transaction's entry, so a fold asks each version's outcome once and every answer
/// agrees (the fold text's 3.1).
pub fn fold_point(io: &mut impl IoDoors) -> Result<Lsn, Error> {
    current().fold_point(&doors(io))
}

/// checkpoint_now requests a fast checkpoint, forced though nothing was written since the last, and waits for it.
pub fn checkpoint_now(io: &mut impl IoDoors) -> Result<(), Error> {
    current().checkpoint_now(&doors(io))
}

/// quiet_database takes a database's lock and ends its sessions, on a hot standby only, as the redo of Postgres' drop
/// of a database does, so that a redo can change the database's files under no session.
pub fn quiet_database(io: &mut impl IoDoors, database: u32) -> Result<(), Error> {
    current().quiet_database(&doors(io), database)
}

/// unquiet_database releases the lock `quiet_database` took, on a hot standby only.
pub fn unquiet_database(io: &mut impl IoDoors, database: u32) -> Result<(), Error> {
    current().unquiet_database(&doors(io), database)
}

/// last_redo returns the redo point of the last checkpoint or restartpoint completed.
pub fn last_redo(io: &mut impl IoDoors) -> Result<Lsn, Error> {
    current().last_redo(&doors(io))
}

/// status says whether a transaction or a subtransaction is in progress, committed, or aborted, in one call, so that
/// Postgres' order of checks, in progress before committed, is the door's and no caller makes them the other way
/// round (design item 7's first transaction-state seam). It checks no range of the xid: every xid a caller asks is
/// one an unfolded version or a pending swap names, which item 7's rule for `relfrozenxid` keeps from the future and
/// from the commit log's truncation.
pub fn status(io: &mut impl IoDoors, xid: Xid) -> Result<Status, Error> {
    current().xid_status(&doors(io), xid)
}

/// in_progress says whether a transaction or a subtransaction is in progress, by the running transactions, for an
/// xid nothing keeps the commit log for (the status table's text, check 10). Postgres' check reads the commit log
/// and `pg_subtrans` only while a running xid at or below the asked one exists, and both are truncated only below
/// every running xid, so it never reads a part they dropped. Every running xid is within 2^31 of the next one, since
/// Postgres stops assigning xids before wraparound reaches a running one, so the host narrows an xid only in that
/// range and answers no for an older one.
pub fn in_progress(io: &mut impl IoDoors, xid: Xid) -> Result<bool, Error> {
    current().xid_in_progress(&doors(io), xid)
}

/// xids_answered says whether `in_progress` answers for every running and prepared transaction: outside recovery, and
/// in a hot standby, whose known-assigned xids hold the primary's, but not in crash or archive recovery, whose proc
/// array holds neither the transactions the crash cut short nor the prepared ones before the end of recovery. There
/// Postgres' check answers yes for every xid only because `latestCompletedXid` is zero until the end of recovery
/// sets it (xlog.c's `StartupXLOG`, "also initialize latestCompletedXid"), which nothing promises, so the fork asks
/// this rather than rest on it (bugs row 90).
pub fn xids_answered() -> bool {
    current().xids_answered()
}

/// removable_in says whether a NonVacuumable snapshot's horizon has passed an xid (seam 2's door for its kind), so
/// that a version the xid deleted is dead to everyone.
pub fn removable_in(io: &mut impl IoDoors, snapshot: &Snapshot<'_>, xid: Xid) -> Result<bool, Error> {
    current().removable_in(&doors(io), snapshot.of(), xid)
}

/// current_writer returns the xid a version the current command writes names as its writer, the current
/// subtransaction's, and its top-level transaction's, which the commit status table keys it by (design item 7's
/// first transaction-state seam), assigning each that has none, Postgres' `GetCurrentFullTransactionId` and
/// `GetTopFullTransactionId`. It raises where Postgres assigns none: in recovery, in a parallel worker, and where the
/// xid limits stop assignment near wraparound.
pub fn current_writer(io: &mut impl IoDoors) -> Result<(Xid, Xid), Error> {
    current().current_writer(&doors(io))
}

/// btree_compare returns how a btree class's comparison function orders two values. It takes the entry's whole `Io`,
/// which every lock guard of the fork's borrows, so it can't be called while one is held: the function is user code
/// (design item 13).
pub fn btree_compare(io: &mut io::Io, compared: &Compared, a: &[u8], b: &[u8]) -> Result<i32, Error> {
    current().btree_compare(&doors(io), compared, a, b)
}

/// reformed re-forms a fold's stored row into its table's layout in the catalog (STORAGE_DESIGN §II.10). It takes the
/// entry's whole `Io`, as `btree_compare` does, so it can't be called while a lock guard of the fork's is held.
pub fn reformed(io: &mut io::Io, row: &ReformRow<'_>) -> Result<ReformedRow, Error> {
    current().reformed(&doors(io), row)
}

/// sort_key returns a text's sort key under a collation. It takes the entry's whole `Io`, as `btree_compare` does, so it
/// can't be called while a lock guard of the fork's is held.
pub fn sort_key(io: &mut io::Io, collation: u32, text: &[u8]) -> Result<Vec<u8>, Error> {
    current().sort_key(&doors(io), collation, text)
}

/// current_command returns the current command's id, Postgres' `GetCurrentCommandId`, marking it used when the
/// command writes, which raises in a parallel worker.
pub fn current_command(io: &mut impl IoDoors, used: bool) -> Result<Command, Error> {
    current().current_command(&doors(io), used)
}

/// is_current reports whether a version's writer is the current transaction or one of its subtransactions that
/// hasn't aborted, Postgres' `TransactionIdIsCurrentTransactionId`, which a version's command is read by. It checks
/// no range of the xid, which an unfolded version names, as `status` checks none.
pub fn is_current(_: &mut impl IoDoors, xid: Xid) -> bool {
    current().xid_is_current(xid)
}

/// combo_command returns the combo command id of a change at `cmax` of a row its own transaction's `writer` wrote at
/// `cmin`, which the change's version shows as `cmin` and `cmax` of the row ((4b)'s §6): heap's
/// `HeapTupleHeaderAdjustCmax`, whose combo table hands out ids in the order of new pairs.
pub fn combo_command(io: &mut impl IoDoors, writer: Xid, cmin: Command, cmax: Command) -> Result<Command, Error> {
    current().combo_command(&doors(io), writer, cmin, cmax)
}

/// STRIPES is the count of the key lock's stripes, each one of Postgres' lightweight locks ((4b)'s key lock).
pub const STRIPES: u32 = 4096;

/// key_count reads a stripe's push count, which a resolve over a per-storage stripe reads before its candidates and
/// again under the stripe, starting over when another writer pushed under the stripe between ((4b)'s §3).
pub fn key_count(stripe: u32) -> u64 {
    current().key_count(stripe % STRIPES)
}

/// Operation is what a wait for a transaction is for, which its error context names: the relation's object id, the
/// row's tuple id, and Postgres' `XLTW_Oper` (`XLTW_Update`, `XLTW_Delete`, `XLTW_Lock`, and the others).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Operation {
    pub relation: u32,
    pub tid: u64,
    pub oper: i32,
}

/// TupleLock is a row's tuple lock: its database, its relation's object id, its tuple id, and the row lock mode, as
/// Postgres' `LockTupleMode` numbers it, whose heavyweight mode heap's `tupleLockExtraInfo` gives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TupleLock {
    pub database: u32,
    pub relation: u32,
    pub tid: u64,
    pub mode: u8,
}

/// xact_wait waits until a transaction ends, as `XactLockTableWait` does, or answers whether it ended with `wait`
/// false; it takes the entry's `Waits`, so it can't be called under a key lock's stripe.
pub fn xact_wait(
    _: &mut io::Waits,
    io: &mut impl IoDoors,
    xid: Xid,
    on: Option<Operation>,
    wait: bool,
) -> Result<bool, Error> {
    current().xact_wait(&doors(io), xid, on, wait)
}

/// speculative_wait waits until a running speculative insert of a transaction with a token completes, as the btree's
/// `_bt_check_unique` waits with `SpeculativeInsertionWait`; it takes the entry's `Waits`, so it can't be called under a
/// key lock's stripe.
pub fn speculative_wait(_: &mut io::Waits, io: &mut impl IoDoors, xid: Xid, token: u32) -> Result<(), Error> {
    current().speculative_wait(&doors(io), xid, token)
}

/// TupleGuard is a row's tuple lock held, released when it drops, as heap releases its tuple lock once its change is
/// made.
pub struct TupleGuard {
    host: &'static dyn Host,
    tuple: TupleLock,
}

/// lock_tuple takes a row's tuple lock, as `heap_acquire_tuplock` does, or with `wait` false answers none when
/// another holds a conflicting one; it takes the entry's `Waits`, so it can't be called under a key lock's stripe.
pub fn lock_tuple(
    _: &mut io::Waits,
    io: &mut impl IoDoors,
    tuple: TupleLock,
    wait: bool,
) -> Result<Option<TupleGuard>, Error> {
    let host = current();
    Ok(host.lock_tuple(&doors(io), tuple, wait)?.then_some(TupleGuard { host, tuple }))
}

impl Drop for TupleGuard {
    fn drop(&mut self) {
        self.host.unlock_tuple(self.tuple);
    }
}

/// wal_stand returns where the server's WAL stands.
pub fn wal_stand(_: &mut impl IoDoors) -> WalStand {
    current().wal_stand()
}

/// warn_kept_wal reads the WAL from one position to another, on a timeline and up to a position, and reports at
/// WARNING, naming the segment, where it can't be read.
pub fn warn_kept_wal(io: &mut impl IoDoors, from: Lsn, to: Lsn, timeline: u32, upto: Lsn) -> Result<(), Error> {
    current().warn_kept_wal(&doors(io), from, to, timeline, upto)
}

/// ForkRecord is one of the fork's WAL records as a read of the WAL finds it: its kind, its end, and its data.
pub type ForkRecord = (u8, Lsn, Vec<u8>);

/// next_fork_record reads the first of the fork's records whose kind is in a mask past a record's end, up to the
/// replayed WAL in recovery and the flushed WAL otherwise (PLAN item 8's "Standbys' roots").
pub fn next_fork_record(io: &mut impl IoDoors, from: Lsn, kinds: u32) -> Result<Option<ForkRecord>, Error> {
    current().next_fork_record(&doors(io), from, kinds)
}

/// warn reports an error at WARNING with a context before its message, for work that goes on past it in place of
/// raising it.
pub fn warn(io: &mut impl IoDoors, error: Error, context: &CStr) -> Result<(), Error> {
    current().warn(&doors(io), error, context)
}

/// log reports a message in the server's log at LOG.
pub fn log(io: &mut impl IoDoors, message: &CStr) -> Result<(), Error> {
    current().log(&doors(io), message)
}

/// refuse_recovery publishes a standby's root mismatch, with the detail of the FATAL that ends the startup process.
pub fn refuse_recovery(_: &mut impl IoDoors, detail: &str) {
    current().refuse_recovery(detail)
}

/// delay_checkpoint opens a window holding off the start of a checkpoint until it finishes, with the names of the
/// pauses inside it, which its open loads.
pub fn delay_checkpoint(io: &mut impl IoDoors, pauses: &[&'static CStr]) -> Result<DelayCheckpoint, Error> {
    let host = current();
    host.delay_checkpoints(&doors(io), pauses)?;
    Ok(DelayCheckpoint { host, finished: false })
}

/// prepare_compressing prepares a zstd dictionary for compressing at a level, failing with Postgres' out of memory
/// error when it can't allocate it.
pub fn prepare_compressing(bytes: &[u8], level: i32) -> Result<CompressDictionary, Error> {
    current().prepare_compressing(bytes, level)
}

/// prepare_decompressing prepares a zstd dictionary for decompressing, failing with Postgres' out of memory error when
/// it can't allocate it.
pub fn prepare_decompressing(bytes: &[u8]) -> Result<DecompressDictionary, Error> {
    reading().prepare_decompressing(bytes)
}

/// compress returns bytes compressed as one zstd frame that records their length, at a level or with a dictionary,
/// failing with Postgres' out of memory error when it can't allocate its memory.
pub fn compress(bytes: &[u8], level: i32, dictionary: Option<&CompressDictionary>) -> Result<Vec<u8>, Error> {
    current().compress(bytes, level, dictionary)
}

/// frame_size returns the length a zstd frame records of its bytes, or None when it records none or is no frame.
pub fn frame_size(frame: &[u8]) -> Option<u64> {
    reading().frame_size(frame)
}

/// decompress returns a zstd frame's `size` bytes, decompressed with a dictionary if it names one, or None when the
/// frame is damaged or holds another size, failing with Postgres' out of memory error when it can't allocate its
/// memory.
pub fn decompress(
    frame: &[u8],
    size: usize,
    dictionary: Option<&DecompressDictionary>,
) -> Result<Option<Vec<u8>>, Error> {
    reading().decompress(frame, size, dictionary)
}

/// train returns a zstd dictionary of at most `capacity` bytes trained on samples, or None when it can't train one
/// on them.
pub fn train(samples: &[&[u8]], capacity: usize) -> Option<Vec<u8>> {
    current().train(samples, capacity)
}

/// backup_begin counts a base backup in and then waits while a collection runs, from `do_pg_backup_start`, so no
/// collection changes the pack lists, the forwards, or the files while the backup copies them (the collector text's
/// §3.3); a cancel ends the wait with Postgres' error, the backup left counted in until `backup_end` counts it out.
pub fn backup_begin(io: &mut Io) -> Result<(), Error> {
    current().backup_begin(&doors(io))
}

/// backup_end counts a base backup out, from `do_pg_backup_stop` and `do_pg_abort_backup`.
pub fn backup_end() {
    current().backup_end()
}

/// backups_running reports whether a base backup is counted in, which a collection reads at its safe points and the
/// removal queue before it removes a file.
pub fn backups_running() -> bool {
    current().backups_running()
}

/// collection_begin sets the collection's word unless a base backup is counted in, answering whether the collection
/// may run; one that may ends with `collection_end`.
pub fn collection_begin() -> bool {
    current().collection_begin()
}

/// collection_end clears the collection's word and wakes the base backups waiting for it.
pub fn collection_end() {
    current().collection_end()
}

/// collection_ask asks the collector worker for a full collection of a database's repository and waits for its answer,
/// the packs it retired, those it copied out, and its live bytes, each `u64::MAX` when it failed (the collector text's
/// §8.2); a cancel ends the wait with Postgres' error, the ask taken back, and the collection goes on.
pub fn collection_ask(io: &mut Io, database: u32) -> Result<[u64; 3], Error> {
    current().collection_ask(&doors(io), database)
}

/// collection_asked returns the unanswered ask's word and the database it asks for, which the collector worker reads.
pub fn collection_asked() -> Option<(u64, u32)> {
    current().collection_asked()
}

/// collection_answer answers an ask, by its word, with the collection's counts, and wakes its asker.
pub fn collection_answer(word: u64, counts: [u64; 3]) {
    current().collection_answer(word, counts)
}

/// collector_started records the running process as the collector worker, whose latch an ask sets.
pub fn collector_started() {
    current().collector_started()
}

/// database_locked takes a database's lock in session mode without waiting, answering whether it took it, so the
/// collector worker opens the database's repository only while no DROP DATABASE or ALTER DATABASE SET TABLESPACE
/// holds or waits for that database's lock, as Postgres' own commands take it.
pub fn database_locked(io: &mut Io, database: u32) -> Result<bool, Error> {
    current().database_locked(&doors(io), database)
}

/// database_unlocked gives back a database's lock that `database_locked` took.
pub fn database_unlocked(database: u32) {
    current().database_unlocked(database)
}

/// database_lock_waited reports whether a session waits for the lock this process holds on a database, which a
/// collection reads at its safe points to stop for a drop or a move of the database.
pub fn database_lock_waited(io: &mut Io, database: u32) -> Result<bool, Error> {
    current().database_lock_waited(&doors(io), database)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Once;
    use std::sync::atomic::{AtomicU64, Ordering};

    /// Test is a host of fixed answers, whose file seams these tests can't reach.
    struct Test {
        /// next is the next unique number.
        next: AtomicU64,
        /// base is the monotonic clock's one reading.
        base: OnceLock<Instant>,
    }

    impl ReadHost for Test {
        fn open_to_read(&self, _: &MayRaise, _: &Path) -> Result<i32, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn read_at(&self, _: &MayRaise, _: i32, _: u64, _: &mut [u8]) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn size(&self, _: &MayRaise, _: i32) -> Result<u64, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn prefetch(&self, _: i32, _: u64, _: u64) {}
        fn close(&self, _: i32) {}
        fn tablespace_root(&self, _: u32) -> PathBuf {
            PathBuf::new()
        }

        fn open_if_there(&self, _: &MayRaise, _: &Path) -> Result<Option<i32>, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn list_dir_if_there(&self, _: &MayRaise, _: &Path) -> Result<Option<Vec<String>>, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn list_dir(&self, _: &MayRaise, _: &Path) -> Result<Vec<String>, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn prepare_decompressing(&self, _: &[u8]) -> Result<DecompressDictionary, Error> {
            unreachable!("these tests compress nothing")
        }

        fn frame_size(&self, _: &[u8]) -> Option<u64> {
            unreachable!("these tests compress nothing")
        }

        fn decompress(&self, _: &[u8], _: usize, _: Option<&DecompressDictionary>) -> Result<Option<Vec<u8>>, Error> {
            unreachable!("these tests compress nothing")
        }
    }

    impl Host for Test {
        fn now(&self) -> SystemTime {
            SystemTime::UNIX_EPOCH + Duration::from_secs(42)
        }

        #[allow(clippy::disallowed_methods)]
        fn instant(&self) -> Instant {
            *self.base.get_or_init(Instant::now)
        }

        fn process_id(&self) -> u32 {
            7
        }

        fn work_mem(&self) -> u64 {
            4096 * 1024
        }

        fn session_database(&self) -> Option<(NonZeroU32, u32)> {
            None
        }

        fn exiting(&self) -> bool {
            false
        }

        fn unique(&self) -> u64 {
            self.next.fetch_add(1, Ordering::Relaxed)
        }

        fn check_interrupts(&self, _: &MayRaise) -> Result<(), Error> {
            unreachable!("these tests make no capability a seam that can raise takes")
        }

        fn hold_interrupts(&self) {}

        fn resume_interrupts(&self) {}

        fn open(&self, _: &MayRaise, _: &Path, _: Open) -> Result<i32, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn open_scratch(&self, _: &MayRaise) -> Result<i32, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn write_at(&self, _: &MayRaise, _: i32, _: u64, _: &[u8]) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn truncate(&self, _: &MayRaise, _: i32, _: u64) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn sync(&self, _: &MayRaise, _: i32) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn rename(&self, _: &MayRaise, _: &Path, _: &Path) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn rename_durably(&self, _: &MayRaise, _: &Path, _: &Path) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn remove_durably(&self, _: &MayRaise, _: &Path) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn remove_durably_if_there(&self, _: &MayRaise, _: &Path) -> Result<bool, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn remove_leftover(&self, _: &Path) {}
        fn rename_back(&self, _: &Path, _: &Path) -> bool {
            false
        }

        fn remove_tree(&self, _: &MayRaise, _: &Path) -> Result<bool, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn create_if_absent(&self, _: &MayRaise, _: &Path) -> Result<Option<i32>, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn sync_path(&self, _: &MayRaise, _: &Path, _: bool) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn make_dir(&self, _: &MayRaise, _: &Path) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn disk_space(&self, _: &MayRaise, _: &Path) -> Result<(u64, u64), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn tree_bytes(&self, _: &MayRaise, _: &Path) -> Result<u64, Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn nap(&self, _: &MayRaise, _: Duration) -> Result<(), Error> {
            unreachable!("these tests make no capability a file seam takes")
        }

        fn wal_insert(&self, _: &MayRaise, _: WalKind, _: &[u8]) -> Result<Lsn, Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn wal_new_cid(&self, _: &MayRaise, _: &Storage, _: Row, _: NewCid) -> Result<Lsn, Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn wal_flush(&self, _: &MayRaise, _: Lsn) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn fold_point(&self, _: &MayRaise) -> Result<Lsn, Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn checkpoint_now(&self, _: &MayRaise) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn quiet_database(&self, _: &MayRaise, _: u32) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn unquiet_database(&self, _: &MayRaise, _: u32) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn last_redo(&self, _: &MayRaise) -> Result<Lsn, Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn wal_stand(&self) -> WalStand {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn warn_kept_wal(&self, _: &MayRaise, _: Lsn, _: Lsn, _: u32, _: Lsn) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn next_fork_record(&self, _: &MayRaise, _: Lsn, _: u32) -> Result<Option<ForkRecord>, Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn warn(&self, _: &MayRaise, _: Error, _: &CStr) -> Result<(), Error> {
            unreachable!("these tests make no capability a report takes")
        }

        fn log(&self, _: &MayRaise, _: &CStr) -> Result<(), Error> {
            unreachable!("these tests make no capability a report takes")
        }

        fn refuse_recovery(&self, _: &str) {
            unreachable!("these tests don't recover")
        }

        fn delay_checkpoints(&self, _: &MayRaise, _: &[&'static CStr]) -> Result<(), Error> {
            unreachable!("these tests make no capability a WAL seam takes")
        }

        fn resume_checkpoints(&self, _: bool) {}

        fn xid_status(&self, _: &MayRaise, _: Xid) -> Result<Status, Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn xid_in_progress(&self, _: &MayRaise, _: Xid) -> Result<bool, Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn xids_answered(&self) -> bool {
            true
        }

        fn removable_in(&self, _: &MayRaise, _: &dyn Any, _: Xid) -> Result<bool, Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn current_writer(&self, _: &MayRaise) -> Result<(Xid, Xid), Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn current_command(&self, _: &MayRaise, _: bool) -> Result<Command, Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn key_lock(&self, _: &MayRaise, _: &[u32]) -> Result<(), Error> {
            unreachable!("these tests make no capability a key lock takes")
        }

        fn key_unlock(&self, _: &[u32], _: bool) {}

        fn key_count(&self, _: u32) -> u64 {
            0
        }

        fn key_hold(&self, _: u32) -> bool {
            false
        }

        fn key_release(&self, _: u32) {}

        fn key_held_wait(&self, _: &MayRaise, _: u32) -> Result<(), Error> {
            unreachable!("these tests make no capability a wait takes")
        }

        fn xact_wait(&self, _: &MayRaise, _: Xid, _: Option<Operation>, _: bool) -> Result<bool, Error> {
            unreachable!("these tests make no capability a wait takes")
        }

        fn lock_tuple(&self, _: &MayRaise, _: TupleLock, _: bool) -> Result<bool, Error> {
            unreachable!("these tests make no capability a wait takes")
        }

        fn speculative_wait(&self, _: &MayRaise, _: Xid, _: u32) -> Result<(), Error> {
            unreachable!("these tests make no capability a wait takes")
        }

        fn unlock_tuple(&self, _: TupleLock) {}

        fn xid_is_current(&self, _: Xid) -> bool {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn combo_command(&self, _: &MayRaise, _: Xid, _: Command, _: Command) -> Result<Command, Error> {
            unreachable!("these tests make no capability a transaction seam takes")
        }

        fn actor(&self) -> Option<u64> {
            None
        }

        fn yield_point(&self) {}
        fn pause(&self, _: &MayRaise, _: &CStr) -> Result<(), Error> {
            unreachable!("these tests make no capability a pause takes")
        }

        fn injected(&self, _: &MayRaise, _: &CStr) -> Result<bool, Error> {
            unreachable!("these tests make no capability an injection point takes")
        }

        fn at_exit(&self, _: TypeId, _: fn(&mut Io) -> Result<(), Error>) -> Result<(), Error> {
            unreachable!("no test of `host` registers an exit work")
        }

        fn prepare_compressing(&self, _: &[u8], _: i32) -> Result<CompressDictionary, Error> {
            unreachable!("these tests compress nothing")
        }

        fn compress(&self, _: &[u8], _: i32, _: Option<&CompressDictionary>) -> Result<Vec<u8>, Error> {
            unreachable!("these tests compress nothing")
        }

        fn train(&self, _: &[&[u8]], _: usize) -> Option<Vec<u8>> {
            unreachable!("these tests compress nothing")
        }

        fn shared_make(&self, _: &MayRaise, _: &shared::Kind) -> Result<Option<u64>, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_publish(&self, _: u64) -> u64 {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_claim(&self, _: &shared::Kind, _: u64) -> Result<Option<u64>, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_free(&self, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_give_up(&self, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_open(&self, _: &MayRaise, _: &shared::Kind, _: u64) -> Result<Option<u64>, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_close(&self, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_reclaim(&self, _: &shared::Kind, _: u64) -> Result<shared::Looked, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_reclaimed(&self, _: u64) -> bool {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_unreclaim(&self, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_sweep(&self, _: &MayRaise, _: &shared::Kind, _: usize) -> Result<Vec<(u64, u64)>, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_single(&self, _: &shared::Kind) -> Result<u64, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_ready(&self) -> bool {
            false
        }

        fn shared_lock(&self, _: &MayRaise, _: u64, _: usize, _: bool) -> Result<(), Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_unlock(&self, _: u64, _: usize) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn flag_take(&self, _: &MayRaise, _: u64, _: usize) -> Result<(), Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn flag_try(&self, _: &MayRaise, _: u64, _: usize) -> Result<bool, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn flag_clear(&self, _: u64, _: usize) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn atomic(&self, _: u64, _: usize, _: shared::Op) -> u64 {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_atomics(&self, _: u64) -> usize {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn blob_read(&self, _: u64, _: usize) -> Vec<u8> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn blob_write(&self, _: u64, _: usize, _: &[u8]) -> Result<(), Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn entry_read(&self, _: u64, _: usize, _: usize) -> Result<Vec<u8>, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn entry_write(&self, _: u64, _: usize, _: usize, _: &[u8]) -> Result<(), Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_location(&self, _: u64) -> u64 {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_live(&self, _: u64) -> bool {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn shared_gone(&self, _: &shared::Kind, _: u64) -> Result<bool, Error> {
            unreachable!("no test of `host` asks whether a record is gone")
        }

        fn slots_name(&self, _: u64, _: usize) -> u64 {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn slots_rename(&self, _: u64, _: usize, _: u64, _: u64) -> bool {
            unreachable!("these tests reach no shared memory")
        }

        fn words_processes(&self, _: &shared::Kind) -> Result<Vec<u32>, Error> {
            unreachable!("these tests reach no shared memory")
        }

        fn slots_empty(&self, _: u64, _: usize) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn slots_claim(&self, _: &MayRaise, _: u64, _: usize, _: &[u8]) -> Result<bool, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn slots_read(&self, _: u64, _: usize) -> Vec<Vec<u8>> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn words_mine(&self, _: &shared::Kind) -> Result<u64, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn word_store(&self, _: u64, _: usize, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn word_store_own(&self, _: usize, _: &shared::Kind, _: usize, _: u64) {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn words_place(&self, _: &shared::Kind, _: u32) -> Result<u64, Error> {
            unreachable!("no test of `host` exchanges a process's word")
        }

        fn word_load(&self, _: &shared::Kind, _: u32, _: usize) -> Result<u64, Error> {
            unreachable!("these tests reach no shared memory")
        }

        fn epochs_lock(&self, _: &MayRaise) -> Result<(), Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn epochs_unlock(&self) {
            unreachable!("these tests reach no shared memory")
        }

        fn epochs_advance(&self) -> u64 {
            unreachable!("these tests reach no shared memory")
        }

        fn epochs_oldest(&self) -> u64 {
            unreachable!("these tests reach no shared memory")
        }

        fn epochs_read(&self, _: &MayRaise) -> Result<u64, Error> {
            unreachable!("these tests make no capability a shared-memory seam takes")
        }

        fn epochs_end(&self, _: u64) {
            unreachable!("these tests reach no shared memory")
        }

        fn proc_number(&self) -> u32 {
            0
        }

        fn delta_hold(&self, _: &MayRaise) -> Result<u64, Error> {
            unreachable!("these tests make no capability a hold takes")
        }

        fn delta_release(&self, _: u64) {
            unreachable!("no test of `host` holds the delta's pages")
        }

        fn delta_open(&self, _: delta::Page, _: u64) -> Result<delta::directory::Opened, Error> {
            unreachable!("no test of `host` holds the delta's pages")
        }

        fn delta_bytes(&self, _: &delta::directory::Opened, _: u64, _: u64) -> Result<cache::Bytes, Error> {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_room(&self, _: &delta::directory::Opened) -> u64 {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_filled(&self, _: &delta::directory::Opened) -> u64 {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_word(&self, _: u64, _: u64, _: delta::WordOp) -> u64 {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_alloc(&self, _: &MayRaise, _: &[delta::Size], _: &mut Vec<delta::Page>) -> Result<Option<u64>, Error> {
            unreachable!("these tests make no capability an allocation takes")
        }

        fn delta_alloc_replacing(
            &self,
            _: &MayRaise,
            _: delta::Size,
            _: delta::Page,
            _: &mut Vec<delta::Page>,
        ) -> Result<Option<u64>, Error> {
            unreachable!("these tests make no capability an allocation takes")
        }

        fn delta_give_back(&self, _: delta::Page) {
            unreachable!("no test of `host` owns a page of the delta")
        }

        fn delta_owned_append(&self, _: delta::Page, _: &[u8]) -> Result<u32, Error> {
            unreachable!("no test of `host` owns a page of the delta")
        }

        fn delta_owned_store(&self, _: delta::Page, _: u64, _: u64) -> Result<(), Error> {
            unreachable!("no test of `host` owns a page of the delta")
        }

        fn delta_link(&self, _: delta::Page, _: delta::directory::Owner) {
            unreachable!("no test of `host` owns a page of the delta")
        }

        fn delta_append(
            &self,
            _: &delta::directory::Opened,
            _: delta::directory::Owner,
            _: &[u8],
        ) -> Result<u32, Error> {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_seal(&self, _: &delta::directory::Opened, _: delta::directory::Owner) -> Result<(), Error> {
            unreachable!("no test of `host` opens a page of the delta")
        }

        fn delta_retire(&self, _: &[delta::Leaving], _: &[delta::Page]) {
            unreachable!("no test of `host` retires a page of the delta")
        }

        fn delta_make(&self, _: &MayRaise, _: &shared::Kind, _: &str) -> Result<Result<(u64, u64), u64>, Error> {
            unreachable!("no test of `host` makes a record of the delta")
        }

        fn delta_record(&self, _: &shared::Kind, _: u64, _: u64) -> Result<u64, Error> {
            unreachable!("no test of `host` opens a record of the delta")
        }

        fn delta_publish(&self, _: u64) {
            unreachable!("no test of `host` makes a record of the delta")
        }

        fn delta_unmake(&self, _: u64) {
            unreachable!("no test of `host` makes a record of the delta")
        }

        fn delta_pass(&self) {
            unreachable!("no test of `host` retires a page of the delta")
        }

        fn delta_advanced(&self) -> (u64, u64) {
            unreachable!("no test of `host` advances the delta's epoch")
        }

        fn delta_used(&self) -> u64 {
            unreachable!("no test of `host` reads the delta's count")
        }

        fn delta_budget(&self) -> u64 {
            unreachable!("no test of `host` reads the delta's budget")
        }

        fn delta_seed(&self) -> [u8; 32] {
            unreachable!("no test of `host` reads the delta's seed")
        }

        fn delta_wait(&self, _: &MayRaise, _: u64) -> Result<(), Error> {
            unreachable!("these tests make no capability a wait takes")
        }

        fn backup_begin(&self, _: &MayRaise) -> Result<(), Error> {
            unreachable!("no test of `host` starts a base backup")
        }

        fn backup_end(&self) {
            unreachable!("no test of `host` ends a base backup")
        }

        fn backups_running(&self) -> bool {
            unreachable!("no test of `host` collects")
        }

        fn collection_begin(&self) -> bool {
            unreachable!("no test of `host` collects")
        }

        fn collection_end(&self) {
            unreachable!("no test of `host` collects")
        }

        fn collection_ask(&self, _: &MayRaise, _: u32) -> Result<[u64; 3], Error> {
            unreachable!("no test of `host` collects")
        }

        fn collection_asked(&self) -> Option<(u64, u32)> {
            unreachable!("no test of `host` collects")
        }

        fn collection_answer(&self, _: u64, _: [u64; 3]) {
            unreachable!("no test of `host` collects")
        }

        fn collector_started(&self) {
            unreachable!("no test of `host` collects")
        }

        fn database_locked(&self, _: &MayRaise, _: u32) -> Result<bool, Error> {
            unreachable!("no test of `host` collects")
        }

        fn database_unlocked(&self, _: u32) {
            unreachable!("no test of `host` collects")
        }

        fn database_lock_waited(&self, _: &MayRaise, _: u32) -> Result<bool, Error> {
            unreachable!("no test of `host` collects")
        }

        fn cache_get(&self, _: &MayRaise, _: cache::Placement, _: &cache::Key) -> Result<Option<cache::Found>, Error> {
            unreachable!("these tests make no capability a node-cache seam takes")
        }

        fn cache_install(
            &self,
            _: &MayRaise,
            _: cache::Placement,
            _: &cache::Key,
            _: usize,
            _: &mut dyn FnMut(&mut [u8]),
        ) -> Result<Option<cache::Found>, Error> {
            unreachable!("these tests make no capability a node-cache seam takes")
        }

        fn cache_contains(&self, _: &MayRaise, _: cache::Placement, _: &cache::Key) -> Result<bool, Error> {
            unreachable!("these tests make no capability a node-cache seam takes")
        }

        fn cache_unpin(&self, _: cache::Placement, _: u32) {
            unreachable!("these tests reach no node cache")
        }

        fn cache_make_local(&self, _: &MayRaise, _: usize) -> Result<(), Error> {
            unreachable!("these tests make no capability a node-cache seam takes")
        }

        fn owner_enlarge(&self, _: &MayRaise) -> Result<u64, Error> {
            unreachable!("these tests make no capability a resource-owner seam takes")
        }

        fn owner_top_enlarge(&self, _: &MayRaise) -> Result<u64, Error> {
            unreachable!("these tests make no capability a resource-owner seam takes")
        }

        fn btree_compare(&self, _: &MayRaise, _: &Compared, _: &[u8], _: &[u8]) -> Result<i32, Error> {
            unreachable!("these tests compare by no class's function")
        }

        fn sort_key(&self, _: &MayRaise, _: u32, _: &[u8]) -> Result<Vec<u8>, Error> {
            unreachable!("these tests sort no text by a collation")
        }

        fn reformed(&self, _: &MayRaise, _: &ReformRow<'_>) -> Result<ReformedRow, Error> {
            unreachable!("these tests re-form no row")
        }

        fn owner_remember(&self, _: u64, _: u64) {
            unreachable!("these tests reach no resource owner")
        }

        fn owner_forget(&self, _: u64, _: u64) {
            unreachable!("these tests reach no resource owner")
        }

        fn owned_remember(&self, _: u64, _: u64, _: fn(u32)) {
            unreachable!("these tests reach no resource owner")
        }

        fn owned_forget(&self, _: u64, _: u64) {
            unreachable!("these tests reach no resource owner")
        }
    }

    /// TEST is the tests' host.
    static TEST: Test = Test { next: AtomicU64::new(0), base: OnceLock::new() };

    /// OTHER is another host.
    static OTHER: Test = Test { next: AtomicU64::new(0), base: OnceLock::new() };

    /// INSTALL installs the tests' host once, whichever test runs first.
    static INSTALL: Once = Once::new();

    /// with_test_host installs the tests' host, if no test has yet.
    fn with_test_host() {
        INSTALL.call_once(|| install(&TEST).expect("no host is installed yet"));
    }

    /// The doors answer from the installed host, and its unique numbers don't repeat.
    #[test]
    fn the_doors_answer_from_the_installed_host() {
        with_test_host();
        assert_eq!(now(), SystemTime::UNIX_EPOCH + Duration::from_secs(42));
        assert_eq!(process_id(), 7);
        assert_ne!(unique(), unique());
    }

    /// A process has one host, so installing another is refused.
    #[test]
    fn a_second_host_is_refused() {
        with_test_host();
        assert!(install(&OTHER).is_err());
    }

    /// The time since a reading after now is zero.
    #[test]
    fn elapsed_saturates() {
        with_test_host();
        assert_eq!(elapsed(instant() + Duration::from_secs(1)), Duration::ZERO);
    }

    /// The process's one backend, which every server process is, keeps one value of a backend-local static.
    #[test]
    fn the_process_keeps_one_backend_local_value() {
        with_test_host();
        static COUNT: local::BackendLocal<AtomicU64> = local::BackendLocal::new();
        COUNT.get().fetch_add(1, Ordering::Relaxed);
        COUNT.get().fetch_add(1, Ordering::Relaxed);
        assert_eq!(COUNT.get().load(Ordering::Relaxed), 2);
    }
}
