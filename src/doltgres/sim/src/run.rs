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

//! Runs of the simulation's actors (design item 14's S3): several backends of one test, each on a thread of its own,
//! of which exactly one holds the run's baton between two yield points. Every door of the simulation's host and every
//! take of a backend-local lock is a yield point, where the seed's generator, or the test's script, picks the actor
//! that runs next. A take of a shared lock, flag, or the epoch lock that another actor holds makes its taker wait, and
//! a run whose live actors all wait is a deadlock, reported with what each waits on and who holds it. Interrupts and
//! terminations wait while an actor holds a shared lock or the epoch lock, or is in a checkpoint-delay window, as
//! Postgres holds interrupts there. A terminated actor stops at a yield point without unwinding. An actor that ends
//! runs its exit works on its own thread, and the run then releases what its exit callbacks would; a crash stops
//! every actor where it is, runs no exit work, and discards their shared records. An actor's
//! id is never reused, so its backend-local values stay unread and are never dropped, as a server process's statics
//! never are.
#![expect(
    clippy::disallowed_methods,
    clippy::disallowed_types,
    reason = "the simulation's own scheduler, whose actors are the test process's threads, for tests alone"
)]

use std::any::TypeId;
use std::collections::{BTreeMap, VecDeque};
use std::ffi::CStr;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::ThreadId;

use host::Error;
use host::io::Io;

use crate::faults::{self, Sim};
use crate::files::{self, Process};
use crate::shared;

/// FIRST_ACTOR is the first id a run's actor takes; `as_actor`'s ids are below it.
pub(crate) const FIRST_ACTOR: u64 = 1 << 20;

/// NEXT_ACTOR is the id the next run's actor takes, which no actor took before.
static NEXT_ACTOR: AtomicU64 = AtomicU64::new(FIRST_ACTOR);

/// Part is a shared part an actor waits for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Part {
    /// Lock is a record's lock, wanted exclusively or shared.
    Lock { place: u64, lock: usize, exclusive: bool },
    /// Flag is a record's flag.
    Flag { place: u64, flag: usize },
    /// Epochs is the epoch lock.
    Epochs,
    /// Budget is the delta's budget, waited at until its count of frees moves on from the one given.
    Budget { frees: u64 },
    /// Stripe is one of the key lock's stripes.
    Stripe { stripe: u32 },
    /// Held is a stripe's hold-back flag, waited at until no other process's is set.
    Held { stripe: u32 },
    /// Ended is a transaction's end, which a wait for the transaction waits on.
    Ended { xid: u64 },
    /// Tuple is a row's tuple lock, wanted in a row lock mode.
    Tuple { tuple: host::TupleLock },
}

/// Step is how far a script's step runs its actor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Step {
    /// Yields is a number of yield points the actor passes.
    Yields(u64),
    /// Until is the name of the pause the actor runs up to.
    Until(&'static CStr),
}

/// Status is where an actor is in its run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Status {
    /// Ready is an actor that runs when the baton comes to it.
    Ready,
    /// Waiting is an actor that runs once the part is free.
    Waiting(Part),
    /// Done is an actor whose body returned.
    Done,
    /// Stopped is an actor terminated or stopped by a crash, whose thread never runs again.
    Stopped,
}

/// Ending is how an actor's run ended.
#[derive(Debug, PartialEq, Eq)]
pub enum Ending {
    /// Returned is the body's result, an error as its SQLSTATE and message.
    Returned(Result<(), ([u8; 5], String)>),
    /// Terminated is an actor the test terminated.
    Terminated,
}

/// Outcome is how a run ended.
#[derive(Debug)]
pub enum Outcome {
    /// Ended is every actor's ending, in the order they were added.
    Ended(Vec<(String, Ending)>),
    /// Crashed is a run the simulation's crash stopped, whose shared records are discarded.
    Crashed,
    /// Failed is a run that can't go on: a deadlock, with each waiting actor's part and its holders, an actor's panic,
    /// or a script that names an actor that can't run.
    Failed(String),
}

/// Report is a run's outcome and the actor that ran at each yield point, which two runs of a seed share; it holds the
/// test process's shared memory until it drops, so the test reads what the run left.
#[derive(Debug)]
pub struct Report {
    /// outcome is how the run ended.
    pub outcome: Outcome,
    /// trace gives the actor at each yield point, in order, by the order the actors were added.
    pub trace: Vec<usize>,
    /// exits gives each exit work that failed: its actor's name, and its error's SQLSTATE and message.
    pub exits: Vec<(String, [u8; 5], String)>,
    /// process holds the test process's shared memory, from the run's.
    _process: Process,
}

/// Slot is an actor of a run.
struct Slot {
    name: String,
    /// id is the actor's id, unique for the process's life.
    id: u64,
    /// thread is the actor's thread, once it starts.
    thread: Option<ThreadId>,
    status: Status,
    /// holds counts the shared locks, epoch lock, and checkpoint-delay windows the actor holds, under which its
    /// interrupts and termination wait.
    holds: u32,
    /// yields counts the yield points the actor passed.
    yields: u64,
    /// terminate_at is the yield count at or after which the actor is terminated, once it holds nothing.
    terminate_at: Option<u64>,
    /// ending is how the actor ended, once it did.
    ending: Option<Ending>,
    /// failed are the errors of the actor's exit works that failed, each a SQLSTATE and a message.
    failed: Vec<([u8; 5], String)>,
}

impl Slot {
    /// due reports whether the actor's termination is due and it holds nothing under which termination waits.
    fn due(&self) -> bool {
        self.terminate_at.is_some_and(|at| self.yields >= at) && self.holds == 0
    }
}

/// Board is a run's state, under the baton's lock.
struct Board {
    slots: Vec<Slot>,
    /// running is the actor that holds the baton, or None while the test does.
    running: Option<usize>,
    /// random is the seeded generator's state.
    random: u64,
    /// script is the steps left of a scripted schedule: an actor, and how far it runs before the next step.
    script: VecDeque<(usize, Step)>,
    /// outcome ends the run early: a crash or a failure.
    outcome: Option<Outcome>,
    trace: Vec<usize>,
    /// check runs at every yield point, its message failing the run there.
    check: Option<fn() -> Option<String>>,
}

/// Baton is a run's board and the condition its actors wait on for their turn.
struct Baton {
    board: Mutex<Board>,
    turn: Condvar,
}

impl Baton {
    /// board takes the run's board.
    fn board(&self) -> MutexGuard<'_, Board> {
        self.board.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// hand gives the baton to an actor, or to the test, and wakes every thread to look.
    fn hand(&self, board: &mut Board, next: Option<usize>) {
        board.running = next;
        self.turn.notify_all();
    }

    /// wait_turn waits until the baton comes to an actor.
    fn wait_turn<'a>(&self, mut board: MutexGuard<'a, Board>, me: usize) -> MutexGuard<'a, Board> {
        while board.running != Some(me) {
            board = self.turn.wait(board).unwrap_or_else(PoisonError::into_inner);
        }
        board
    }

    /// park stops an actor's thread for good, as a process that ended or crashed never runs again.
    fn park(&self, mut board: MutexGuard<'_, Board>) -> ! {
        loop {
            board = self.turn.wait(board).unwrap_or_else(PoisonError::into_inner);
        }
    }
}

/// proc returns an actor's process number, as the simulation's shared memory names it.
fn proc(id: u64) -> u32 {
    u32::try_from(id + 1).expect("actors are numbered below 2^32")
}

impl Board {
    /// runnable reports whether an actor can run: it is ready, or waits on a part that is free or with an interrupt
    /// due that ends a flag's wait.
    fn runnable(&self, at: usize) -> bool {
        let slot = &self.slots[at];
        match slot.status {
            Status::Ready => true,
            Status::Waiting(part) => {
                shared::free_for(part, proc(slot.id))
                    || matches!(part, Part::Flag { .. } | Part::Budget { .. } | Part::Ended { .. } | Part::Tuple { .. })
                        && (slot.due() || slot.thread.is_some_and(files::interrupt_due))
            }
            Status::Done | Status::Stopped => false,
        }
    }

    /// choose picks the actor that runs next, by the script while it has steps and by the seed after, or None when no
    /// actor can run, which fails the run as a deadlock if any waits.
    fn choose(&mut self) -> Option<usize> {
        let runnable: Vec<usize> = (0..self.slots.len()).filter(|&at| self.runnable(at)).collect();
        while let Some(&(actor, step)) = self.script.front() {
            let name = &self.slots[actor].name;
            match (self.slots[actor].status, step) {
                (Status::Done | Status::Stopped, Step::Yields(_)) => {
                    self.script.pop_front();
                }
                (Status::Done | Status::Stopped, Step::Until(point)) => {
                    let failed = format!("the script runs {name} to its pause at {point:?}, which it never reached");
                    self.outcome = Some(Outcome::Failed(failed));
                    return None;
                }
                _ if runnable.contains(&actor) => return Some(actor),
                (_, Step::Yields(_)) => {
                    self.outcome = Some(Outcome::Failed(format!("the script runs {name}, which waits")));
                    return None;
                }
                (_, Step::Until(point)) => {
                    let failed = format!("the script runs {name} to its pause at {point:?}, before which it waits");
                    self.outcome = Some(Outcome::Failed(failed));
                    return None;
                }
            }
        }
        if runnable.is_empty() {
            if self.slots.iter().any(|slot| matches!(slot.status, Status::Waiting(_))) {
                self.outcome = Some(Outcome::Failed(self.deadlock()));
            }
            return None;
        }
        Some(runnable[(faults::next(&mut self.random) % runnable.len() as u64) as usize])
    }

    /// deadlock describes a deadlock: each waiting actor with the part it waits on and the actors that hold it.
    fn deadlock(&self) -> String {
        let name = |held: u32| {
            self.slots.iter().find(|slot| proc(slot.id) == held).map_or(format!("process {held}"), |s| s.name.clone())
        };
        let waits: Vec<String> = self
            .slots
            .iter()
            .filter_map(|slot| match slot.status {
                Status::Waiting(part) => {
                    let holders: Vec<String> = shared::holders(part).into_iter().map(name).collect();
                    Some(format!("{} waits on {part:?}, held by {}", slot.name, holders.join(" and ")))
                }
                _ => None,
            })
            .collect();
        format!("deadlock: {}", waits.join("; "))
    }

    /// stop ends an actor that was terminated or finished: it releases what its process's exit callbacks would, its
    /// node cache pins through the code the server's exit callback runs, as the actor.
    fn stop(&mut self, at: usize, status: Status, ending: Ending) {
        files::acting_as(self.slots[at].id, host::cache::release_all);
        shared::exit(proc(self.slots[at].id));
        self.slots[at].status = status;
        self.slots[at].ending = Some(ending);
    }
}

/// RUNNING are the threads of the runs' actors, each with its run and its place in it.
static RUNNING: Mutex<Vec<(ThreadId, Arc<Baton>, usize)>> = Mutex::new(Vec::new());

/// mine returns the run and place of the running thread, if it runs as a run's actor.
fn mine() -> Option<(Arc<Baton>, usize)> {
    let thread = std::thread::current().id();
    let running = RUNNING.lock().unwrap_or_else(PoisonError::into_inner);
    running.iter().find(|(actor, ..)| *actor == thread).map(|(_, baton, at)| (baton.clone(), *at))
}

/// yield_point lets the run hand the baton to another actor, and stops an actor whose termination is due and who
/// holds nothing.
pub(crate) fn yield_point() {
    yielded(None);
}

/// pause is a yield point that carries a name, where a script's step that runs its actor to that name ends.
pub(crate) fn pause(point: &CStr) {
    yielded(Some(point));
}

/// yielded passes a yield point, at a named pause or not.
fn yielded(point: Option<&CStr>) {
    let Some((baton, me)) = mine() else { return };
    let mut board = baton.board();
    board.slots[me].yields += 1;
    board.trace.push(me);
    if board.slots[me].due() {
        terminate(&baton, board, me);
    }
    let ended = match board.script.front_mut() {
        Some((actor, Step::Yields(left))) if *actor == me => {
            *left = left.saturating_sub(1);
            *left == 0
        }
        Some((actor, Step::Until(until))) if *actor == me => point == Some(*until),
        _ => false,
    };
    if ended {
        board.script.pop_front();
    }
    let next = match board.check.and_then(|check| check()) {
        Some(failed) => {
            board.outcome = Some(Outcome::Failed(failed));
            None
        }
        None => board.choose(),
    };
    if next != Some(me) {
        baton.hand(&mut board, next);
        drop(baton.wait_turn(board, me));
    }
}

/// Work is an actor's exit work: the type that names it, and its function.
type Work = (TypeId, fn(&mut Io) -> Result<(), Error>);

/// Exits are an actor's exit works, the newest last, and whether its exit has begun, after which it lists none.
#[derive(Default)]
struct Exits {
    works: Vec<Work>,
    begun: bool,
}

/// EXITS are the actors' exit works, by actor.
static EXITS: Mutex<BTreeMap<u64, Exits>> = Mutex::new(BTreeMap::new());

/// at_exit lists an exit work of the running actor, once however often it is asked, and refuses one asked once the
/// actor's exit has begun. A thread that is no actor never exits, so its work isn't listed.
pub(crate) fn at_exit(work: TypeId, run: fn(&mut Io) -> Result<(), Error>) -> Result<(), Error> {
    let Some(actor) = files::running_actor() else { return Ok(()) };
    let mut exits = EXITS.lock().unwrap_or_else(PoisonError::into_inner);
    let exits = exits.entry(actor).or_default();
    if exits.begun {
        return Err(Error::fork(b"XX000", "an exit work was registered after its process's exit began".to_owned()));
    }
    if exits.works.iter().all(|(listed, _)| *listed != work) {
        exits.works.push((work, run));
    }
    Ok(())
}

/// exiting returns whether the running actor's exit has begun; a thread that is no actor never exits.
pub(crate) fn exiting() -> bool {
    let Some(actor) = files::running_actor() else { return false };
    EXITS.lock().unwrap_or_else(PoisonError::into_inner).get(&actor).is_some_and(|exits| exits.begun)
}

/// exit_works runs the running actor's exit works on its own thread, the newest first and each whatever the others
/// returned, clearing the flags the actor holds before each, as the server's exit callback does, and returns the
/// errors of those that failed. A termination can't end the actor again while they run.
fn exit_works(id: u64) -> Vec<([u8; 5], String)> {
    if let Some((baton, me)) = mine() {
        baton.board().slots[me].terminate_at = None;
    }
    let mut failed = Vec::new();
    loop {
        shared::clear_flags(proc(id));
        let newest = {
            let mut exits = EXITS.lock().unwrap_or_else(PoisonError::into_inner);
            let exits = exits.entry(id).or_default();
            exits.begun = true;
            exits.works.pop()
        };
        let Some((_, work)) = newest else { return failed };
        if let Err(err) = work(&mut Io::testing()) {
            failed.push((*err.sqlstate(), err.message().to_owned()));
        }
    }
}

/// terminate stops an actor whose termination is due: it runs the actor's exit works from where it stopped, with
/// the frames below abandoned, releases what its exit callbacks would, and parks its thread.
fn terminate(baton: &Baton, mut board: MutexGuard<'_, Board>, me: usize) -> ! {
    let id = board.slots[me].id;
    board.slots[me].status = Status::Ready;
    drop(board);
    let failed = exit_works(id);
    let mut board = baton.board();
    board.slots[me].failed = failed;
    board.stop(me, Status::Stopped, Ending::Terminated);
    let next = board.choose();
    baton.hand(&mut board, next);
    baton.park(board)
}

/// wait makes the running actor wait for a part another actor holds, returning false outside a run, where a wait
/// would never end. The wait is a yield point. A script's step for the actor ends at its wait, unless an interrupt or
/// a termination ends the wait at once, and a due termination ends a flag's wait, as `pg_terminate_backend` ends an
/// interruptible one.
pub(crate) fn wait(part: Part) -> bool {
    let Some((baton, me)) = mine() else { return false };
    let mut board = baton.board();
    board.slots[me].yields += 1;
    board.trace.push(me);
    board.slots[me].status = Status::Waiting(part);
    if board.script.front().is_some_and(|&(actor, step)| actor == me && matches!(step, Step::Yields(_)))
        && !board.runnable(me)
    {
        board.script.pop_front();
    }
    let next = board.choose();
    if next != Some(me) {
        baton.hand(&mut board, next);
        board = baton.wait_turn(board, me);
    }
    if matches!(
        part,
        Part::Flag { .. } | Part::Budget { .. } | Part::Ended { .. } | Part::Tuple { .. } | Part::Held { .. }
    ) && board.slots[me].due()
    {
        terminate(&baton, board, me);
    }
    board.slots[me].status = Status::Ready;
    true
}

/// hold counts a shared lock, the epoch lock, or a checkpoint-delay window the running actor takes or leaves.
pub(crate) fn hold(taken: bool) {
    if let Some((baton, me)) = mine() {
        let holds = &mut baton.board().slots[me].holds;
        *holds = if taken { *holds + 1 } else { holds.saturating_sub(1) };
    }
}

/// holding reports whether the running actor holds a part under which its interrupts wait.
pub(crate) fn holding() -> bool {
    mine().is_some_and(|(baton, me)| baton.board().slots[me].holds > 0)
}

/// crash stops every actor of the running actor's run where it is, as a PANIC's restart runs no drops and no exit
/// callbacks; outside a run it does nothing, and the door fails with the crash.
pub(crate) fn crash() {
    let Some((baton, me)) = mine() else { return };
    let mut board = baton.board();
    board.slots[me].status = Status::Stopped;
    board.outcome = Some(Outcome::Crashed);
    baton.hand(&mut board, None);
    baton.park(board);
}

/// Body is an actor's work, run with its own capability.
type Body = Box<dyn FnOnce(&mut Io) -> Result<(), Error> + Send>;

/// Run is a run of actors, seeded, that holds the test process's shared memory until it drops.
pub struct Run {
    baton: Arc<Baton>,
    bodies: Vec<Body>,
    /// sim is the simulation the test's thread started, whose files, clock, and numbers the actors share.
    sim: Option<Arc<Sim>>,
    /// process holds the test process's shared memory for the run's life.
    _process: Process,
}

impl Run {
    /// new starts a run whose schedule a seed picks.
    pub fn new(seed: u64) -> Run {
        let board = Board {
            slots: Vec::new(),
            running: None,
            random: seed | 1,
            script: VecDeque::new(),
            outcome: None,
            trace: Vec::new(),
            check: None,
        };
        crate::install();
        let run = Run {
            baton: Arc::new(Baton { board: Mutex::new(board), turn: Condvar::new() }),
            bodies: Vec::new(),
            sim: faults::current(),
            _process: files::process(),
        };
        crate::delta::seed_delta(seed);
        run
    }

    /// actor adds an actor, which runs a body with its own capability.
    pub fn actor(&mut self, name: &str, body: impl FnOnce(&mut Io) -> Result<(), Error> + Send + 'static) {
        let slot = Slot {
            name: name.to_owned(),
            id: NEXT_ACTOR.fetch_add(1, Ordering::Relaxed),
            thread: None,
            status: Status::Ready,
            holds: 0,
            yields: 0,
            terminate_at: None,
            ending: None,
            failed: Vec::new(),
        };
        self.baton.board().slots.push(slot);
        self.bodies.push(Box::new(body));
    }

    /// at returns an actor's place, by its name.
    fn at(board: &Board, name: &str) -> usize {
        board.slots.iter().position(|slot| slot.name == name).expect("a run names only the actors it added")
    }

    /// script runs the actors in steps, each an actor and the yield points it passes before the next step, or until it
    /// waits, ahead of the seed's schedule; a step whose actor waits when it starts fails the run.
    pub fn script(&mut self, steps: &[(&str, u64)]) {
        let mut board = self.baton.board();
        let steps: Vec<(usize, Step)> =
            steps.iter().map(|&(name, yields)| (Run::at(&board, name), Step::Yields(yields))).collect();
        board.script.extend(steps);
    }

    /// until adds a step to the script that runs an actor up to its next pause of a name, where it stays until a
    /// later step runs it on; an actor that ends or waits before it reaches the pause fails the run.
    pub fn until(&mut self, name: &str, point: &'static CStr) {
        let mut board = self.baton.board();
        let at = Run::at(&board, name);
        board.script.push_back((at, Step::Until(point)));
    }

    /// check runs a check at every yield point of the run's actors, whose message fails the run there.
    pub fn check(&mut self, check: fn() -> Option<String>) {
        self.baton.board().check = Some(check);
    }

    /// terminate_at terminates an actor, as `pg_terminate_backend` does, at its yield point after it passed a number
    /// of them and once it holds nothing.
    pub fn terminate_at(&mut self, name: &str, yields: u64) {
        let mut board = self.baton.board();
        let at = Run::at(&board, name);
        board.slots[at].terminate_at = Some(yields);
    }

    /// finish runs the actors until every one ended, or the run crashed or failed, and reports how. A crash starts the
    /// process's shared memory afresh, as a restart reinitializes it, and a failure discards what its actors that never
    /// ended left, so no later run waits on them.
    pub fn finish(self) -> Report {
        let Run { baton, bodies, sim, _process } = self;
        for (at, body) in bodies.into_iter().enumerate() {
            let (baton, sim) = (baton.clone(), sim.clone());
            std::thread::spawn(move || act(&baton, at, sim, body));
        }
        let mut board = baton.board();
        let first = board.choose();
        baton.hand(&mut board, first);
        while board.running.is_some() {
            board = baton.turn.wait(board).unwrap_or_else(PoisonError::into_inner);
        }
        let exits = board
            .slots
            .iter_mut()
            .flat_map(|slot| {
                let name = slot.name.clone();
                slot.failed.drain(..).map(move |(state, message)| (name.clone(), state, message))
            })
            .collect();
        let outcome = match board.outcome.take() {
            Some(Outcome::Crashed) => {
                shared::reset();
                Outcome::Crashed
            }
            Some(failed) => {
                let stuck = board.slots.iter().filter(|slot| slot.status != Status::Done);
                shared::discard(&stuck.map(|slot| proc(slot.id)).collect::<Vec<u32>>());
                failed
            }
            None => Outcome::Ended(
                board
                    .slots
                    .iter_mut()
                    .map(|slot| {
                        (std::mem::take(&mut slot.name), slot.ending.take().expect("every actor of an ended run ended"))
                    })
                    .collect(),
            ),
        };
        Report { outcome, trace: std::mem::take(&mut board.trace), exits, _process }
    }
}

/// act is an actor's thread: it waits for its first turn, runs its body as its actor, and at its end releases what
/// its exit callbacks would and hands the baton on.
fn act(baton: &Arc<Baton>, me: usize, sim: Option<Arc<Sim>>, body: Body) {
    let thread = std::thread::current().id();
    faults::adopt(sim);
    RUNNING.lock().unwrap_or_else(PoisonError::into_inner).push((thread, baton.clone(), me));
    let id = {
        let mut board = baton.board();
        board.slots[me].thread = Some(thread);
        let board = baton.wait_turn(board, me);
        board.slots[me].id
    };
    let result = files::acting_as(id, || {
        catch_unwind(AssertUnwindSafe(|| {
            let result = body(&mut Io::testing());
            (result, exit_works(id))
        }))
    });
    let mut board = baton.board();
    match result {
        Ok((result, failed)) => {
            let result = result.map_err(|err| (*err.sqlstate(), err.message().to_owned()));
            board.slots[me].failed = failed;
            board.stop(me, Status::Done, Ending::Returned(result));
            let next = board.choose();
            baton.hand(&mut board, next);
        }
        Err(panic) => {
            let message = panic
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| panic.downcast_ref::<&str>().map(|text| (*text).to_owned()))
                .unwrap_or_default();
            let failed = format!("{} panicked: {message}", board.slots[me].name);
            board.slots[me].status = Status::Stopped;
            board.outcome = Some(Outcome::Failed(failed));
            baton.hand(&mut board, None);
        }
    }
    drop(board);
    RUNNING.lock().unwrap_or_else(PoisonError::into_inner).retain(|(actor, ..)| *actor != thread);
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
    use std::sync::{Arc, Mutex};

    use host::io::Io;
    use host::local::BackendLocal;
    use host::lock::{IoMutex, LongMutex};
    use host::shared::{self, After, Atomic, BlobAt, Flag, Held, Limits, Lock, Op, Pool, Shape};
    use host::{Error, Open};

    use super::{Ending, Outcome, Report, Run};

    /// Counter is the tests' record: two locks either may be taken under the other, a flag, an atomic, and a blob.
    struct Counter;

    impl Shape for Counter {
        const ID: u64 = 0x7e57_5300;
        const LOCKS: usize = 2;
        const FLAGS: usize = 1;
        const ATOMICS: usize = 1;
        const BLOBS: &'static [usize] = &[16];
        const ARRAYS: &'static [(usize, usize)] = &[];
    }

    impl Pool for Counter {
        const NAME: &'static str = "counter";

        fn count(_: &Limits) -> usize {
            1024
        }

        fn reclaim(_: &mut Io) -> Result<bool, Error> {
            Ok(false)
        }
    }

    /// First is the first lock.
    struct First;
    impl Lock<Counter> for First {
        const INDEX: usize = 0;
    }
    impl After<Second> for First {}

    /// Second is the second lock, which these tests alone also take before the first, to make a deadlock.
    struct Second;
    impl Lock<Counter> for Second {
        const INDEX: usize = 1;
    }
    impl After<First> for Second {}

    /// Busy is the flag.
    struct Busy;
    impl Flag<Counter> for Busy {
        const INDEX: usize = 0;
    }

    /// Count is the atomic.
    struct Count;
    impl Atomic<Counter> for Count {
        const INDEX: usize = 0;
    }

    /// BYTES is the index of the blob, under the first lock.
    const BYTES: usize = 0;
    impl BlobAt<BYTES> for Counter {
        type Guard = First;
    }

    /// published makes a record and publishes it, from the test's thread.
    fn published() -> u64 {
        let io = &mut Io::testing();
        shared::make::<Counter>(io).unwrap().publish()
    }

    /// count reads a record's atomic, from the test's thread.
    fn count(location: u64) -> u64 {
        let io = &mut Io::testing();
        opened(io, location).unwrap().record().atomic::<Count>(Op::Load)
    }

    /// opened opens a record the tests published.
    fn opened(io: &mut Io, location: u64) -> Result<Held<Counter>, Error> {
        shared::open::<Counter>(io, location)?.ok_or_else(|| failed("the location names no record"))
    }

    /// failed returns the fork's internal error with a message, for an actor whose check failed.
    fn failed(message: &str) -> Error {
        Error::fork(b"XX000", message.to_owned())
    }

    /// ended returns each actor's ending of a run that ended.
    fn ended(report: &Report) -> &[(String, Ending)] {
        match &report.outcome {
            Outcome::Ended(endings) => endings,
            other => panic!("the run didn't end: {other:?}"),
        }
    }

    /// counted runs two actors that each add one to a record's atomic three times under its first lock, by reading
    /// it and writing it back, and returns the total and the run's trace.
    fn counted(seed: u64) -> (u64, Vec<usize>) {
        let mut run = Run::new(seed);
        let location = published();
        for name in ["a", "b"] {
            run.actor(name, move |io| {
                let held = opened(io, location)?;
                let record = held.record();
                for _ in 0..3 {
                    let _first = record.lock::<First>(io)?;
                    let value = record.atomic::<Count>(Op::Load);
                    record.atomic::<Count>(Op::Exchange(value, value + 1));
                }
                Ok(())
            });
        }
        let mut report = run.finish();
        let trace = std::mem::take(&mut report.trace);
        assert!(ended(&report).iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))));
        (count(location), trace)
    }

    /// Every seed's interleaving of two actors' increments under one lock gives the same total, two runs of a seed
    /// share their trace, and the seeds don't all interleave alike.
    #[test]
    fn a_seed_picks_the_interleaving_and_repeats_it() {
        let runs: Vec<(u64, Vec<usize>)> = (1..=12).map(counted).collect();
        assert!(runs.iter().all(|(total, _)| *total == 6), "a lock let an increment through: {runs:?}");
        assert_eq!(counted(7).1, runs[6].1, "two runs of one seed traced apart");
        assert!(runs.iter().any(|(_, trace)| *trace != runs[0].1), "every seed interleaved alike");
    }

    /// A script runs A into a lock and then B to the same lock, where B waits until A releases it, and B then sees
    /// what A did under it.
    #[test]
    fn a_script_runs_one_actor_into_a_lock_another_waits_on() {
        let mut run = Run::new(1);
        let location = published();
        let seen = Arc::new(AtomicU64::new(0));
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let _first = record.lock::<First>(io)?;
            record.atomic::<Count>(Op::Add(1));
            Ok(())
        });
        let saw = seen.clone();
        run.actor("b", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let _first = record.lock::<First>(io)?;
            saw.store(record.atomic::<Count>(Op::Load), Ordering::Relaxed);
            Ok(())
        });
        run.script(&[("a", 4), ("b", u64::MAX), ("a", u64::MAX)]);
        let report = run.finish();
        assert_eq!(report.trace[..7], [0, 0, 0, 0, 1, 1, 1]);
        assert!(ended(&report).iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))));
        assert_eq!(seen.load(Ordering::Relaxed), 1, "b took the lock before a released it");
    }

    /// Two actors that take two locks in opposite orders deadlock, and the run reports each one's wait and holder.
    #[test]
    fn opposite_lock_orders_deadlock_and_the_run_reports_both_waits() {
        let mut run = Run::new(3);
        let location = published();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let mut first = record.lock::<First>(io)?;
            first.lock::<Second>()?;
            Ok(())
        });
        run.actor("b", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let mut second = record.lock::<Second>(io)?;
            second.lock::<First>()?;
            Ok(())
        });
        run.script(&[("a", 3), ("b", u64::MAX)]);
        let Outcome::Failed(message) = run.finish().outcome else { panic!("a deadlock wasn't reported") };
        assert!(message.starts_with("deadlock: "), "{message}");
        assert!(message.contains("a waits on Lock") && message.contains("b waits on Lock"), "{message}");
        assert!(message.contains("held by b") && message.contains("held by a"), "{message}");
    }

    /// An actor waiting on a flag another holds is interrupted by a planned cancel and ends with 57014.
    #[test]
    fn a_planned_cancel_ends_a_flag_wait() {
        let mut run = Run::new(5);
        let location = published();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let _busy = record.flag::<Busy>(io)?;
            for _ in 0..8 {
                record.atomic::<Count>(Op::Load);
            }
            Ok(())
        });
        run.actor("b", move |io| {
            crate::interrupt_at(1);
            let held = opened(io, location)?;
            let record = held.record();
            record.flag::<Busy>(io).map(drop)
        });
        run.script(&[("a", 4), ("b", u64::MAX)]);
        let report = run.finish();
        let endings = ended(&report);
        let cancel = (*b"57014", "canceling statement due to user request".to_owned());
        assert_eq!(endings[1].1, Ending::Returned(Err(cancel)));
    }

    /// A terminated actor's flag is cleared, its waiter takes the flag, and a record it held open is gone once it is
    /// freed.
    #[test]
    fn a_terminated_actors_flag_and_hold_go_with_it() {
        let mut run = Run::new(9);
        let location = published();
        let freed = published();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let _kept = opened(io, freed)?;
            let _busy = record.flag::<Busy>(io)?;
            loop {
                record.atomic::<Count>(Op::Load);
            }
        });
        run.actor("b", move |io| {
            {
                let held = opened(io, location)?;
                let record = held.record();
                drop(record.flag::<Busy>(io)?);
            }
            let owned = shared::claim::<Counter>(io, freed)?.ok_or_else(|| failed("the record wasn't claimed"))?;
            shared::free(owned);
            Ok(())
        });
        run.script(&[("a", 4), ("b", u64::MAX)]);
        run.terminate_at("a", 6);
        let report = run.finish();
        let endings = ended(&report);
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!(endings[1].1, Ending::Returned(Ok(())));
        assert_eq!(crate::shared::state(freed), shared::word::State::Free, "a terminated actor's hold kept a record");
    }

    /// A termination due while an actor waits on a flag ends the wait at once, as `pg_terminate_backend` ends an
    /// interruptible wait, so its exit gives up the claim the flag's holder waits for.
    #[test]
    fn a_due_termination_ends_a_flag_wait() {
        let mut run = Run::new(31);
        let location = published();
        let claimed = published();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            let _busy = record.flag::<Busy>(io)?;
            let other = &mut Io::testing();
            for _ in 0..50 {
                if let Some(owned) = shared::claim::<Counter>(other, claimed)? {
                    shared::free(owned);
                    return Ok(());
                }
            }
            Err(failed("a termination didn't end a flag wait"))
        });
        run.actor("b", move |io| {
            let _claim = shared::claim::<Counter>(io, claimed)?;
            let held = opened(io, location)?;
            let record = held.record();
            record.flag::<Busy>(io).map(drop)
        });
        run.script(&[("b", 3), ("a", 3), ("b", u64::MAX)]);
        run.terminate_at("b", 4);
        let report = run.finish();
        let endings = ended(&report);
        assert_eq!(endings[0].1, Ending::Returned(Ok(())));
        assert_eq!(endings[1].1, Ending::Terminated);
    }

    /// Each actor keeps its own backend-local value across the yields between its changes.
    #[test]
    fn each_actor_keeps_its_own_backend_local_value_across_yields() {
        static VALUE: BackendLocal<AtomicU64> = BackendLocal::new();
        let mut run = Run::new(11);
        for (name, by) in [("a", 1), ("b", 10), ("c", 100)] {
            run.actor(name, move |_| {
                for step in 1..=4 {
                    VALUE.get().fetch_add(by, Ordering::Relaxed);
                    host::unique();
                    if VALUE.get().load(Ordering::Relaxed) != by * step {
                        return Err(failed("an actor saw another's backend-local value"));
                    }
                }
                Ok(())
            });
        }
        assert!(ended(&run.finish()).iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))));
    }

    /// A termination planned while an actor holds a shared lock takes effect only after it releases the lock, and a
    /// cancel planned in a checkpoint-delay window fires at the first check after the window.
    #[test]
    fn interrupts_and_terminations_wait_while_an_actor_holds_interrupts() {
        let mut run = Run::new(13);
        let location = published();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            {
                let _first = record.lock::<First>(io)?;
                for _ in 0..3 {
                    record.atomic::<Count>(Op::Add(1));
                }
            }
            record.atomic::<Count>(Op::Add(100));
            Ok(())
        });
        run.actor("b", |io| {
            let window = host::delay_checkpoint(io, &[])?;
            crate::interrupt_at(0);
            if io.check().is_err() {
                return Err(failed("a cancel fired inside a checkpoint-delay window"));
            }
            window.finish();
            match io.check() {
                Err(err) if err.sqlstate() == b"57014" => Ok(()),
                _ => Err(failed("a cancel planned in a window didn't fire after it")),
            }
        });
        run.script(&[("a", u64::MAX), ("b", u64::MAX)]);
        run.terminate_at("a", 4);
        let report = run.finish();
        let endings = ended(&report);
        assert_eq!(endings[0].1, Ending::Terminated);
        assert_eq!(endings[1].1, Ending::Returned(Ok(())));
        assert_eq!(count(location), 3, "a termination took effect under a shared lock, or not at its release");
    }

    /// SHARED_STATE is a plain static lock, which two actors share as no backend-local state may be.
    static SHARED_STATE: LongMutex<u64> = LongMutex::new(0);

    /// A plain static lock taken by two actors fails the run, naming where the lock is declared.
    #[test]
    fn a_static_lock_two_actors_take_fails_the_run() {
        let mut run = Run::new(17);
        run.actor("a", |io| {
            let mut held = SHARED_STATE.lock(io);
            *held += 1;
            host::open_to_read(held.io(), std::path::Path::new("/")).map(drop)
        });
        run.actor("b", |io| {
            *SHARED_STATE.lock(io) += 1;
            Ok(())
        });
        run.script(&[("a", 2), ("b", 2)]);
        let Outcome::Failed(message) = run.finish().outcome else { panic!("shared static state wasn't caught") };
        assert!(
            message.starts_with("b panicked: an actor took the storage lock declared at sim/src/run.rs:"),
            "{message}"
        );
        assert!(message.contains("state shared between backends outside shared memory"), "{message}");
    }

    /// Marked is a backend-local value that holds a record open and records whether it was ever dropped.
    #[derive(Default)]
    struct Marked {
        held: IoMutex<Option<Held<Counter>>>,
    }

    /// DROPPED records that a `Marked` was dropped, which no exit of an actor does.
    static DROPPED: AtomicBool = AtomicBool::new(false);

    impl Drop for Marked {
        fn drop(&mut self) {
            DROPPED.store(true, Ordering::Relaxed);
        }
    }

    /// A terminated actor's exit gives up what it has: the record its backend-local value holds open is closed though
    /// the value is never dropped, the record it made and never published is freed, and its claim on a published
    /// record is given up for another to claim.
    #[test]
    fn a_terminated_actors_exit_releases_what_it_owns() {
        static MARKED: BackendLocal<Marked> = BackendLocal::new();
        let mut run = Run::new(19);
        let (claimed, kept) = (published(), published());
        let free = crate::shared::free_slots();
        run.actor("a", move |io| {
            *MARKED.get().held.lock(io) = Some(opened(io, kept)?);
            let _made = shared::make::<Counter>(io)?;
            let _claim = shared::claim::<Counter>(io, claimed)?;
            loop {
                host::unique();
            }
        });
        run.terminate_at("a", 8);
        let report = run.finish();
        let endings = ended(&report);
        assert_eq!(endings[0].1, Ending::Terminated);
        assert!(!DROPPED.load(Ordering::Relaxed), "a terminated actor's backend-local value was dropped");
        assert_eq!(crate::shared::free_slots(), free, "a terminated actor's unpublished record stayed");
        let io = &mut Io::testing();
        assert!(shared::claim::<Counter>(io, claimed).unwrap().is_some(), "a terminated actor's claim stayed");
        shared::free(shared::claim::<Counter>(io, kept).unwrap().expect("a published record is claimed"));
        assert_eq!(crate::shared::state(kept), shared::word::State::Free, "a terminated actor's hold stayed");
    }

    /// A script runs an actor to a named pause, past its pauses of other names, where it stays while another actor
    /// runs past it; an actor that ends before the pause its script names fails the run, with the name.
    #[test]
    fn a_script_runs_an_actor_to_a_named_pause() {
        let mut run = Run::new(1);
        let location = published();
        let seen = Arc::new(AtomicU64::new(u64::MAX));
        let sees = seen.clone();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            host::pause!(io, c"doltgres-sim-before")?;
            held.record().atomic::<Count>(Op::Add(1));
            host::pause!(io, c"doltgres-sim-between")?;
            held.record().atomic::<Count>(Op::Add(10));
            Ok(())
        });
        run.actor("b", move |io| {
            sees.store(opened(io, location)?.record().atomic::<Count>(Op::Load), Ordering::Relaxed);
            Ok(())
        });
        run.until("a", c"doltgres-sim-between");
        run.script(&[("b", u64::MAX), ("a", u64::MAX)]);
        let report = run.finish();
        assert!(ended(&report).iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        assert_eq!((seen.load(Ordering::Relaxed), count(location)), (1, 11));
        let mut run = Run::new(1);
        run.actor("a", move |io| opened(io, location).map(drop));
        run.until("a", c"doltgres-sim-nowhere");
        let failed = run.finish();
        assert!(matches!(&failed.outcome, Outcome::Failed(why) if why.contains("doltgres-sim-nowhere")), "{failed:?}");
    }

    /// A crash with two actors mid-operation, one holding a flag and writing files and one waiting on the flag, stops
    /// both; the files recover to what was synced, shared memory starts afresh, so the record the test made and an
    /// actor wrote is gone with the one the run made, and a new run's actors see their backend-local values'
    /// defaults.
    #[test]
    fn a_crash_stops_every_actor_and_a_new_run_starts_afresh() {
        static WRITES: BackendLocal<AtomicU64> = BackendLocal::new();
        let dir = std::env::temp_dir().join(format!("sim-run-crash-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let sim = crate::Sim::start(&dir, 23);
        let path: PathBuf = dir.join("file");
        let mut run = Run::new(23);
        let location = published();
        let made = Arc::new(Mutex::new(None));
        let maker = made.clone();
        let file = path.clone();
        run.actor("a", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            *maker.lock().unwrap() = Some(shared::make::<Counter>(io)?.publish());
            let mut busy = record.flag::<Busy>(io)?;
            let io = &mut busy.io();
            io.lock::<First>()?.write_blob::<BYTES>(b"written")?;
            let handle = host::open(io, &file, Open::CreateNew)?;
            host::write_at(io, &handle, 0, b"synced")?;
            host::sync(io, &handle)?;
            host::sync_path(io, file.parent().unwrap(), true)?;
            loop {
                WRITES.get().fetch_add(1, Ordering::Relaxed);
                host::write_at(io, &handle, 6, b" lost")?;
            }
        });
        run.actor("b", move |io| {
            let held = opened(io, location)?;
            let record = held.record();
            record.flag::<Busy>(io).map(drop)
        });
        run.script(&[("a", 6), ("b", u64::MAX), ("a", u64::MAX)]);
        sim.crash_at(sim.calls() + 6);
        let crashed = run.finish();
        assert!(matches!(crashed.outcome, Outcome::Crashed));
        sim.recover(false).unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"synced");
        let gone = made.lock().unwrap().expect("a made its record before the crash");
        assert_eq!(crate::shared::state(gone), shared::word::State::Free, "a crashed run's record stayed");
        assert_eq!(crate::shared::state(location), shared::word::State::Free, "a record made before the crash stayed");
        let mut run = Run::new(29);
        run.actor("c", move |io| {
            if WRITES.get().load(Ordering::Relaxed) != 0 {
                return Err(failed("a new run's actor read a crashed actor's backend-local value"));
            }
            match shared::open::<Counter>(io, location)? {
                Some(_) => Err(failed("a record made before a crash was opened after it")),
                None => Ok(()),
            }
        });
        let report = run.finish();
        assert!(ended(&report).iter().all(|(_, ending)| *ending == Ending::Returned(Ok(()))), "{report:?}");
        sim.stop();
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
