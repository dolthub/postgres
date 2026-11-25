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

//! The simulation's transactions (design item 7's first transaction-state seam): the run's table of xids, which a
//! test begins, commits, and aborts, and whose commit log it truncates, which the status and in_progress doors
//! answer from as Postgres' running transactions and commit log answer the server's; and each actor's own
//! transaction, which its savepoints nest and whose xids and command the current doors give.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::thread::ThreadId;

use host::{Command, Error, SnapshotKind, Status, Xid};

/// FIRST is the first xid a test process gives, Postgres' first normal one.
const FIRST: u64 = 3;

/// CACHED is how many of a transaction's subtransactions in progress Postgres' proc array caches before it marks the
/// cache overflowed, `PGPROC_MAX_CACHED_SUBXIDS`.
const CACHED: usize = 64;

/// Xids is the table: each xid begun, with its status and the transaction it is a subtransaction of; the next xid to
/// give; the oldest xid the commit log keeps; the top-level transactions whose cache of subtransactions overflowed, a
/// mark Postgres only sets while a transaction runs; and each actor's own transaction, the process's under no actor.
struct Xids {
    begun: BTreeMap<u64, (Status, Option<u64>)>,
    next: u64,
    kept: u64,
    overflowed: BTreeSet<u64>,
    own: BTreeMap<Option<u64>, Own>,
}

/// Own is an actor's transaction: the xid of each of its levels, the top-level transaction's first and the current
/// subtransaction's last, none where none was assigned yet; its command; and whether its command was used.
struct Own {
    levels: Vec<Option<u64>>,
    command: u32,
    used: bool,
    /// The combo command ids the transaction gave, each its pair of commands at its id's place.
    combos: Vec<(u32, u32)>,
}

impl Own {
    /// fresh returns a transaction with no xid at its first command.
    fn fresh() -> Own {
        Own { levels: vec![None], command: 0, used: false, combos: Vec::new() }
    }
}

/// XIDS is the test process's transactions.
static XIDS: Mutex<Xids> = Mutex::new(Xids {
    begun: BTreeMap::new(),
    next: FIRST,
    kept: FIRST,
    overflowed: BTreeSet::new(),
    own: BTreeMap::new(),
});

/// xids takes the table.
fn xids() -> MutexGuard<'static, Xids> {
    XIDS.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Xids {
    /// begin gives the next xid to a transaction in progress, a subtransaction of another when one is named, and
    /// marks its top-level transaction overflowed once more than `CACHED` of its subtransactions are in progress.
    fn begin(&mut self, parent: Option<Xid>) -> Xid {
        let xid = self.next;
        self.next += 1;
        self.begun.insert(xid, (Status::InProgress, parent.map(|parent| parent.0)));
        if parent.is_some() {
            let top = self.top(xid);
            let running = self
                .begun
                .iter()
                .filter(|&(&each, (status, _))| each != top && *status == Status::InProgress && self.under(each, top));
            if running.count() > CACHED {
                self.overflowed.insert(top);
            }
        }
        Xid(xid)
    }

    /// top returns the top-level transaction an xid belongs to, itself when it is one.
    fn top(&self, mut xid: u64) -> u64 {
        while let Some((_, Some(parent))) = self.begun.get(&xid) {
            xid = *parent;
        }
        xid
    }

    /// under reports whether an xid is another or one of its subtransactions, however deep.
    fn under(&self, mut xid: u64, top: u64) -> bool {
        loop {
            if xid == top {
                return true;
            }
            match self.begun.get(&xid) {
                Some((_, Some(parent))) => xid = *parent,
                _ => return false,
            }
        }
    }

    /// end gives a transaction and each of its subtransactions still in progress an outcome, looking only from its own
    /// xid on, since a subtransaction begins after its parent.
    fn end(&mut self, xid: Xid, outcome: Status) {
        let ended: Vec<u64> =
            self.begun.range(xid.0..).map(|(&each, _)| each).filter(|&each| self.under(each, xid.0)).collect();
        for each in ended {
            if let Some((status, _)) = self.begun.get_mut(&each)
                && *status == Status::InProgress
            {
                *status = outcome;
            }
        }
    }
}

/// begin starts a transaction and returns its xid.
pub fn begin() -> Xid {
    xids().begin(None)
}

/// begin_under starts a subtransaction of a transaction and returns its xid; it stays in progress, after a release
/// too, until it is aborted or its top-level transaction ends.
pub fn begin_under(parent: Xid) -> Xid {
    xids().begin(Some(parent))
}

/// commit commits a transaction, with its subtransactions still in progress.
pub fn commit(xid: Xid) {
    xids().end(xid, Status::Committed);
}

/// abort aborts a transaction or a subtransaction, with its subtransactions still in progress.
pub fn abort(xid: Xid) {
    xids().end(xid, Status::Aborted);
}

/// own takes the running actor's own transaction, a fresh one where it has none, with the table.
fn own<T>(with: impl FnOnce(&mut Xids, &mut Own) -> T) -> T {
    let mut xids = xids();
    let actor = crate::files::running_actor();
    let mut own = xids.own.remove(&actor).unwrap_or_else(Own::fresh);
    let result = with(&mut xids, &mut own);
    xids.own.insert(actor, own);
    result
}

/// savepoint starts a subtransaction of the running actor's transaction, with no xid until one is asked for.
pub fn savepoint() {
    own(|_, own| own.levels.push(None));
}

/// release leaves the running actor's current subtransaction, whose xid stays in progress until its transaction
/// ends.
pub fn release() {
    own(|_, own| {
        if own.levels.len() > 1 {
            own.levels.pop();
        }
    });
}

/// rollback_to aborts the running actor's current subtransaction, with what it released, and leaves it.
pub fn rollback_to() {
    own(|xids, own| {
        if own.levels.len() > 1
            && let Some(Some(xid)) = own.levels.pop()
        {
            xids.end(Xid(xid), Status::Aborted);
        }
    });
}

/// next_command ends the running actor's command, moving to the next command where it was used, as Postgres'
/// `CommandCounterIncrement` does.
pub fn next_command() {
    own(|_, own| {
        if own.used {
            own.command += 1;
            own.used = false;
        }
    });
}

/// end commits or aborts the running actor's transaction, and starts a fresh one.
pub fn end(committed: bool) {
    own(|xids, own| {
        if let Some(top) = own.levels[0] {
            xids.end(Xid(top), if committed { Status::Committed } else { Status::Aborted });
        }
        *own = Own::fresh();
    });
}

/// current_writer answers the current_writer door: the running actor's current subtransaction's xid and its
/// top-level one, assigning each level that has none, the outermost first.
pub(crate) fn current_writer() -> (Xid, Xid) {
    own(|xids, own| {
        let mut parent = None;
        for level in own.levels.iter_mut() {
            let xid = *level.get_or_insert_with(|| xids.begin(parent).0);
            parent = Some(Xid(xid));
        }
        let top = own.levels[0].unwrap_or_default();
        (parent.unwrap_or(Xid(top)), Xid(top))
    })
}

/// current_command answers the current_command door: the running actor's command, marked used when it writes.
pub(crate) fn current_command(used: bool) -> u32 {
    own(|_, own| {
        own.used |= used;
        own.command
    })
}

/// combo_command answers the combo_command door as Postgres' combo table does: a pair of commands the transaction
/// gave an id keeps it, and a new pair takes the next.
pub(crate) fn combo_command(cmin: u32, cmax: u32) -> u32 {
    own(|_, own| match own.combos.iter().position(|&pair| pair == (cmin, cmax)) {
        Some(at) => at as u32,
        None => {
            own.combos.push((cmin, cmax));
            own.combos.len() as u32 - 1
        }
    })
}

/// is_current answers the is_current door: whether an xid is under the running actor's top-level transaction and
/// still in progress, as a released subtransaction's is and an aborted one's isn't.
pub(crate) fn is_current(xid: Xid) -> bool {
    own(|xids, own| own.levels[0].is_some_and(|top| xids.under(xid.0, top) && state(xids, xid) == Status::InProgress))
}

/// Taken is a snapshot the simulation took of its transactions, as Postgres' `GetSnapshotData` takes one, which
/// lends seam 2's value: its kind, its bounds, the running actor's command, whether it is recovery's shape, whether a
/// running transaction's subtransactions overflowed, its running xids and subtransaction xids, 32 bits each, and a
/// NonVacuumable one's horizon, below which it finds every xid removable.
pub struct Taken {
    kind: SnapshotKind,
    horizon: Option<u64>,
    xmin: u64,
    xmax: u64,
    command: u32,
    recovery: bool,
    overflowed: bool,
    xip: Vec<u32>,
    subxip: Vec<u32>,
}

impl Taken {
    /// removable reports whether the snapshot's horizon has passed an xid.
    pub(crate) fn removable(&self, xid: Xid) -> bool {
        xid.0 < self.horizon.expect("only a NonVacuumable snapshot is asked its horizon")
    }

    /// lent lends the snapshot as seam 2's value.
    pub fn lent(&self) -> host::Snapshot<'_> {
        host::Snapshot::made(
            self.kind,
            (Xid(self.xmin), Xid(self.xmax)),
            Command(self.command),
            (self.recovery, self.overflowed),
            (&self.xip, &self.subxip),
            self,
        )
    }
}

/// snapshot takes a snapshot of a kind of the table for the running actor, as `GetSnapshotData` does: its `xmax`
/// the next xid, its `xmin` the oldest running top-level xid else `xmax`, and every running xid but the actor's own
/// transaction's, top-level ones in `xip` and subtransactions in `subxip` unless their transaction overflowed, which
/// marks the snapshot overflowed; in recovery's shape every running xid is in `subxip` and `xip` is empty.
pub fn snapshot(kind: SnapshotKind, recovery: bool) -> Taken {
    own(|xids, own| {
        let mine = own.levels[0];
        let running: Vec<u64> = xids
            .begun
            .iter()
            .filter(|&(&xid, (status, _))| {
                *status == Status::InProgress && mine.is_none_or(|top| !xids.under(xid, top))
            })
            .map(|(&xid, _)| xid)
            .collect();
        let tops: Vec<u64> = running.iter().copied().filter(|&xid| xids.top(xid) == xid).collect();
        let overflowed = tops.iter().any(|top| xids.overflowed.contains(top));
        let narrow = |xid: &u64| *xid as u32;
        let (xip, subxip) = match recovery {
            true => (Vec::new(), running.iter().map(narrow).collect()),
            false => (
                tops.iter().map(narrow).collect(),
                running
                    .iter()
                    .filter(|&&xid| xids.top(xid) != xid && !xids.overflowed.contains(&xids.top(xid)))
                    .map(narrow)
                    .collect(),
            ),
        };
        let xmax = xids.next;
        let xmin = tops.first().copied().unwrap_or(xmax);
        Taken { kind, horizon: None, xmin, xmax, command: own.command, recovery, overflowed, xip, subxip }
    })
}

/// historic takes a historic snapshot, as logical decoding builds one for the transaction it replays: its bounds,
/// the xids that committed between them in `xip`, the decoded transaction's own xids in `subxip`, each sorted as
/// heap's search of them needs, and the command the replay reached.
pub fn historic((xmin, xmax): (Xid, Xid), committed: &[Xid], own: &[Xid], command: Command) -> Taken {
    let sorted = |xids: &[Xid]| {
        let mut narrow: Vec<u32> = xids.iter().map(|xid| xid.0 as u32).collect();
        narrow.sort_unstable();
        narrow
    };
    Taken {
        kind: SnapshotKind::HistoricMvcc,
        horizon: None,
        xmin: xmin.0,
        xmax: xmax.0,
        command: command.0,
        recovery: false,
        overflowed: false,
        xip: sorted(committed),
        subxip: sorted(own),
    }
}

/// non_vacuumable takes a NonVacuumable snapshot whose horizon is the caller's, as Postgres'
/// `InitNonVacuumableSnapshot` takes its caller's.
pub fn non_vacuumable(horizon: Xid) -> Taken {
    let mut taken = snapshot(SnapshotKind::NonVacuumable, false);
    taken.horizon = Some(horizon.0);
    taken
}

/// UNANSWERED is the threads that stand for a process in crash recovery, whose proc array doesn't answer for every
/// running transaction.
static UNANSWERED: Mutex<Vec<ThreadId>> = Mutex::new(Vec::new());

/// unanswered makes the running thread stand for a process in crash recovery or for one out of it.
pub fn unanswered(recovering: bool) {
    let thread = std::thread::current().id();
    let mut unanswered = UNANSWERED.lock().unwrap_or_else(PoisonError::into_inner);
    unanswered.retain(|&other| other != thread);
    if recovering {
        unanswered.push(thread);
    }
}

/// answered answers the xids_answered door.
pub(crate) fn answered() -> bool {
    let thread = std::thread::current().id();
    !UNANSWERED.lock().unwrap_or_else(PoisonError::into_inner).contains(&thread)
}

/// truncate_commit_log makes an xid the oldest the commit log keeps, as a VACUUM that freezes moves it.
pub fn truncate_commit_log(oldest: Xid) {
    let mut xids = xids();
    xids.kept = xids.kept.max(oldest.0);
}

/// state returns an xid's status in the table, aborted for one the table never began, as Postgres reads an xid its
/// commit log holds no commit for; the bootstrap and frozen xids are committed and the invalid one aborted with no
/// read, as Postgres' `TransactionLogFetch` answers them.
fn state(xids: &Xids, xid: Xid) -> Status {
    match xid.0 {
        0 => Status::Aborted,
        1 | 2 => Status::Committed,
        _ => xids.begun.get(&xid.0).map_or(Status::Aborted, |(status, _)| *status),
    }
}

/// status answers the status door: an xid's status, by the running transactions and then the commit log, whose read
/// of an xid older than it keeps fails as Postgres' read of a removed segment does.
pub(crate) fn status(xid: Xid) -> Result<Status, Error> {
    let xids = xids();
    match state(&xids, xid) {
        Status::InProgress => Ok(Status::InProgress),
        _ if (FIRST..xids.kept).contains(&xid.0) => Err(Error::fork(
            b"58P01",
            format!("could not access status of transaction {}", xid.0 & u64::from(u32::MAX)),
        )),
        ended => Ok(ended),
    }
}

/// in_progress answers the in_progress door as Postgres' `TransactionIdIsInProgress` does: by the running
/// transactions, reading the commit log only in its step 4, while a running transaction at or below the xid has an
/// overflowed cache, where a read of an xid older than the commit log keeps fails as Postgres' read does.
pub(crate) fn in_progress(xid: Xid) -> Result<bool, Error> {
    let xids = xids();
    if state(&xids, xid) == Status::InProgress {
        return Ok(true);
    }
    let step_4 = xids.overflowed.iter().any(|&top| top <= xid.0 && state(&xids, Xid(top)) == Status::InProgress);
    if step_4 && (FIRST..xids.kept).contains(&xid.0) {
        return Err(Error::fork(
            b"58P01",
            format!("could not access status of transaction {}", xid.0 & u64::from(u32::MAX)),
        ));
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// asked returns what the status door says of an xid through the test's capability, which holds the process, so
    /// that no other test's transactions run meanwhile.
    fn asked(io: &mut crate::TestIo, xid: Xid) -> Result<Status, Error> {
        host::status(&mut **io, xid)
    }

    /// The seam's contract (design item 7): a transaction is in progress until it ends, and then committed or
    /// aborted.
    #[test]
    fn a_transaction_is_in_progress_until_it_ends() {
        let io = &mut crate::io();
        let (one, other) = (begin(), begin());
        assert_eq!(asked(io, one).unwrap(), Status::InProgress);
        assert_eq!(asked(io, other).unwrap(), Status::InProgress);
        commit(one);
        abort(other);
        assert_eq!(asked(io, one).unwrap(), Status::Committed);
        assert_eq!(asked(io, other).unwrap(), Status::Aborted);
    }

    /// The seam's contract: a subtransaction rolled back is aborted while its transaction runs on, and one released
    /// is in progress until its transaction ends, with its outcome.
    #[test]
    fn a_subtransaction_ends_by_its_rollback_or_with_its_transaction() {
        let io = &mut crate::io();
        let top = begin();
        let (rolled_back, released, deep) = (begin_under(top), begin_under(top), begin_under(top));
        let deeper = begin_under(deep);
        abort(rolled_back);
        assert_eq!(asked(io, rolled_back).unwrap(), Status::Aborted);
        assert_eq!(asked(io, released).unwrap(), Status::InProgress);
        assert_eq!(asked(io, top).unwrap(), Status::InProgress);
        abort(deep);
        assert_eq!(asked(io, deeper).unwrap(), Status::Aborted);
        commit(top);
        assert_eq!(asked(io, top).unwrap(), Status::Committed);
        assert_eq!(asked(io, released).unwrap(), Status::Committed);
        assert_eq!(asked(io, rolled_back).unwrap(), Status::Aborted);
        assert_eq!(asked(io, deeper).unwrap(), Status::Aborted);
        let aborted = begin();
        let under = begin_under(aborted);
        abort(aborted);
        assert_eq!(asked(io, under).unwrap(), Status::Aborted);
    }

    /// The seam's contract (the status table's text, check 10): an xid the commit log no longer keeps is in progress
    /// to no one, asked alone, while its status read fails as Postgres' read of a removed segment does, and a running
    /// transaction is in progress.
    #[test]
    fn an_xid_the_commit_log_dropped_is_in_progress_to_no_one() {
        let io = &mut crate::io();
        let (old, running) = (begin(), begin());
        commit(old);
        truncate_commit_log(running);
        assert!(!host::in_progress(&mut **io, old).unwrap());
        assert!(host::in_progress(&mut **io, running).unwrap());
        let error = asked(io, old).unwrap_err();
        assert_eq!(error.sqlstate(), b"58P01");
        let refused = format!("could not access status of transaction {}", old.0);
        assert_eq!(error.message(), refused.as_str());
        assert_eq!(asked(io, running).unwrap(), Status::InProgress);
        commit(running);
        assert!(!host::in_progress(&mut **io, running).unwrap());
        assert_eq!(asked(io, running).unwrap(), Status::Committed);
    }

    /// The seam's contract (the status table's text, check 10): Postgres reads the commit log for an xid it doesn't
    /// find running only while a running transaction at or below it has an overflowed cache, so an xid the commit log
    /// dropped is in progress to no one with no read while an overflowed transaction runs above it, and a newer one is
    /// read and found ended.
    #[test]
    fn an_overflowed_transaction_above_a_dropped_xid_reads_no_commit_log() {
        let io = &mut crate::io();
        let old = begin();
        commit(old);
        let top = begin();
        let under: Vec<Xid> = (0..=CACHED).map(|_| begin_under(top)).collect();
        let newer = begin();
        commit(newer);
        truncate_commit_log(top);
        assert!(!host::in_progress(&mut **io, old).unwrap());
        assert!(!host::in_progress(&mut **io, newer).unwrap());
        assert!(host::in_progress(&mut **io, top).unwrap());
        assert!(host::in_progress(&mut **io, under[CACHED]).unwrap());
        commit(top);
        assert!(!host::in_progress(&mut **io, under[0]).unwrap());
    }

    /// The seam's contract (design item 7's first transaction-state seam): a savepoint's writer is its own xid under
    /// its transaction's, assigned outermost first; both are the current transaction's while it runs, and a
    /// subtransaction rolled back is no longer; another transaction's xid never is; a command that writes is followed
    /// by the next command, and one that only reads by itself.
    #[test]
    fn the_current_transaction_gives_its_writer_and_its_command() {
        let io = &mut crate::io();
        let other = begin();
        savepoint();
        let (writer, top) = host::current_writer(&mut **io).unwrap();
        assert!(writer.0 == top.0 + 1 && other.0 < top.0);
        assert_eq!(host::current_writer(&mut **io).unwrap(), (writer, top));
        assert!(host::is_current(&mut **io, writer) && host::is_current(&mut **io, top));
        assert!(!host::is_current(&mut **io, other));
        rollback_to();
        assert!(!host::is_current(&mut **io, writer) && host::is_current(&mut **io, top));
        assert_eq!(host::current_writer(&mut **io).unwrap(), (top, top));
        assert_eq!(host::current_command(&mut **io, false).unwrap(), host::Command(0));
        next_command();
        assert_eq!(host::current_command(&mut **io, true).unwrap(), host::Command(0));
        next_command();
        assert_eq!(host::current_command(&mut **io, false).unwrap(), host::Command(1));
        end(true);
        assert_eq!(asked(io, top).unwrap(), Status::Committed);
        assert_eq!(asked(io, writer).unwrap(), Status::Aborted);
        assert!(!host::is_current(&mut **io, top));
        assert_eq!(host::current_command(&mut **io, false).unwrap(), host::Command(0));
        abort(other);
    }

    /// The seam's contract (PRevPF 5's case for a version no transaction wrote): the bootstrap and frozen xids are
    /// committed and the invalid one aborted, each in progress to no one, with no read of the commit log, even once it
    /// keeps no xid below a running one.
    #[test]
    fn the_xids_no_transaction_has_are_answered_with_no_read() {
        let io = &mut crate::io();
        let running = begin();
        truncate_commit_log(running);
        for (xid, status) in [(0, Status::Aborted), (1, Status::Committed), (2, Status::Committed)] {
            assert_eq!(asked(io, Xid(xid)).unwrap(), status, "xid {xid}");
            assert!(!host::in_progress(&mut **io, Xid(xid)).unwrap(), "xid {xid}");
        }
        commit(running);
    }
}
