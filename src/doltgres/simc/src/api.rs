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

//! sim's test API as the C tests call it (`simc/simc.h`): each function the Rust tests call of `sim`'s, exported under
//! `simc_`, with sim's values the C holds in boxes simc made, which their drops free.

use std::ffi::{CStr, c_char, c_void};
use std::mem::MaybeUninit;
use std::num::NonZeroU32;
use std::sync::{Arc, Mutex, Once, PoisonError};

use host::{Command, Xid};
use sim::{Run, Sim};

use crate::c;

use crate::entry::entered;
use crate::{ffi, table};

/// INSTALL installs the simulation's host as the C port's host once, whichever test runs first.
static INSTALL: Once = Once::new();

/// handed moves a value into a box the C holds, which the matching `taken` takes back to drop.
#[allow(clippy::disallowed_methods)]
pub(crate) fn handed<T>(value: T) -> *mut c_void {
    Box::into_raw(Box::new(value)).cast()
}

/// taken takes back a box `handed` gave the C.
///
/// # Safety
///
/// `boxed` is a box of `T`'s that `handed` gave the C, which the C hands back once.
pub(crate) unsafe fn taken<T>(boxed: *mut c_void) -> Box<T> {
    // SAFETY: the caller's contract, a box `handed` made of a `T`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { Box::from_raw(boxed.cast::<T>()) }
}

/// simc_io is `sim::io`: it installs the simulation's host, in Rust and as the C port's host, if no test has yet, and
/// returns a test's capability, the C's own `Io::testing` with the hold of the test process.
#[unsafe(no_mangle)]
pub extern "C" fn simc_io() -> ffi::SimcTestIo {
    entered!({
        let test = sim::io();
        INSTALL.call_once(|| {
            c::instant_base();
            let host = ffi::HostHost { ops: &table::HOST_OPS, self_: std::ptr::null_mut() };
            let mut refused = ffi::HostRefused { unused: 0 };
            // SAFETY: `host_install` keeps the `'static` table and its null self, which no entry reads. Tested by
            // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
            let installed = unsafe { ffi::host_install(host, &mut refused) };
            assert!(installed, "no host is installed in a test module but simc's");
        });
        // SAFETY: `host_io_Io_testing` makes the C's capability around nothing. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        ffi::SimcTestIo { io: unsafe { ffi::host_io_Io_testing() }, process: handed(test) }
    })
}

/// simc_TestIo_drop is TestIo's drop: its hold of the test process released.
///
/// # Safety
///
/// `test_io` is a `SimcTestIo` that `simc_io` returned, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_TestIo_drop(test_io: *mut c_void) {
    entered!({
        // SAFETY: the caller's contract, a test's capability `simc_io` made, whose box holds a `TestIo`. Tested by
        // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
        drop(unsafe { taken::<sim::TestIo>((*test_io.cast::<ffi::SimcTestIo>()).process) });
    })
}

/// simc_process is `sim::process`: a hold of the test process, which a test takes before its capability so the process
/// outlives it, until `simc_Process_drop` releases it.
#[unsafe(no_mangle)]
pub extern "C" fn simc_process() -> ffi::SimcProcess {
    entered!(ffi::SimcProcess { f0: handed(sim::process()) })
}

/// simc_Process_drop is a hold of the test process's drop.
///
/// # Safety
///
/// `process` is a hold `simc_process` returned, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Process_drop(process: *mut c_void) {
    // SAFETY: the caller's contract, a hold whose box holds a `Process`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability.
    entered!(drop(unsafe { taken::<sim::Process>((*process.cast::<ffi::SimcProcess>()).f0) }))
}

/// simc_tempdir is `sim::tempdir`: a new directory under the system's temporary directory, or the error making it.
///
/// # Safety
///
/// `result` and `error` are places for the answer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_tempdir(result: *mut ffi::SimcTempDir, error: *mut ffi::StdIoError) -> bool {
    entered!(match sim::tempdir() {
        Ok(dir) => {
            // SAFETY: the caller's contract, a place for the directory. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { result.write(ffi::SimcTempDir { f0: handed(dir) }) };
            true
        }
        Err(failure) => {
            // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_test_directory_the_operating_system_cant_make_fails_with_its_error.
            unsafe { error.write(c::io_error(&failure)) };
            false
        }
    })
}

/// simc_TempDir_path is `TempDir::path`, its path's bytes and their length.
///
/// # Safety
///
/// `dir` is a directory `simc_tempdir` made, not yet dropped, which outlives the bytes' use, and `len` a place for
/// their length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_TempDir_path(dir: *const ffi::SimcTempDir, len: *mut usize) -> *const c_char {
    entered!({
        // SAFETY: the caller's contract, a live directory whose box holds a `TempDir`. Tested by
        // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
        let bytes = unsafe { &*(*dir).f0.cast::<sim::TempDir>() }.path().as_os_str().as_encoded_bytes();
        // SAFETY: the caller's contract, a place for the length. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        unsafe { len.write(bytes.len()) };
        bytes.as_ptr().cast()
    })
}

/// simc_TempDir_drop is TempDir's drop: the directory and what it holds removed.
///
/// # Safety
///
/// `dir` is a directory `simc_tempdir` made, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_TempDir_drop(dir: *mut c_void) {
    // SAFETY: the caller's contract, a directory whose box holds a `TempDir`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    entered!(drop(unsafe { taken::<sim::TempDir>((*dir.cast::<ffi::SimcTempDir>()).f0) }))
}

/// simc_interrupt_at is `sim::interrupt_at`: the running thread's interrupts pending once its checks reach a count.
#[unsafe(no_mangle)]
pub extern "C" fn simc_interrupt_at(checks: u64) {
    entered!(sim::interrupt_at(checks))
}

/// simc_begin_owner is `sim::begin_owner`: a subtransaction's resource owner starts inside the running process's
/// innermost one, as a savepoint does.
#[unsafe(no_mangle)]
pub extern "C" fn simc_begin_owner() {
    entered!(sim::begin_owner())
}

/// simc_abort_owner is `sim::abort_owner`: the innermost resource owner ends and releases what it remembered, as a
/// rollback to a savepoint does, or the outermost one's, as a transaction's abort does.
#[unsafe(no_mangle)]
pub extern "C" fn simc_abort_owner() {
    entered!(sim::abort_owner())
}

/// simc_napped is `sim::napped`: how long the naps asked to wait since the last call, the count started again.
#[unsafe(no_mangle)]
pub extern "C" fn simc_napped() -> ffi::StdDuration {
    entered!(c::duration(sim::napped()))
}

/// simc_set_disk_space is `sim::set_disk_space`: the simulated volume's free bytes and its size, for a test of the
/// collector's low disk.
#[unsafe(no_mangle)]
pub extern "C" fn simc_set_disk_space(free: u64, total: u64) {
    entered!(sim::set_disk_space(free, total))
}

/// simc_refuse_owned_append is `sim::refuse_owned_append`: this thread's owned page append after a count of them
/// fails, as an area's refusal would.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_owned_append(after: u64) {
    entered!(sim::refuse_owned_append(after))
}

/// simc_refuse_open is `sim::refuse_open`: this thread's open of a page's name after a count of its opens fails, as a
/// stale name's would.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_open(name: ffi::HostDeltaPagePage, after: u64) {
    entered!(sim::refuse_open(c::page(name), after))
}

/// simc_refuse_record is `sim::refuse_record`: this thread's open of an aged record of a shape at a location after a
/// count of its opens fails, as a stale location's would.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_record(shape: u64, location: u64, after: u64) {
    entered!(sim::refuse_record(shape, location, after))
}

/// simc_refuse_any_open is `sim::refuse_any_open`: this thread's open of any page after a count of them fails.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_any_open(after: u64) {
    entered!(sim::refuse_any_open(after))
}

/// simc_refuse_any_take is `sim::refuse_any_take`: this thread's page allocation or record make after a count of them
/// fails.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_any_take(after: u64) {
    entered!(sim::refuse_any_take(after))
}

/// simc_refuse_nothing is `sim::refuse_nothing`: every refusal this thread set that hasn't come is taken back.
#[unsafe(no_mangle)]
pub extern "C" fn simc_refuse_nothing() {
    entered!(sim::refuse_nothing())
}

/// simc_begin is `sim::begin`: a transaction started, its xid returned.
#[unsafe(no_mangle)]
pub extern "C" fn simc_begin() -> ffi::HostXid {
    entered!(ffi::HostXid { f0: sim::begin().0 })
}

/// simc_begin_under is `sim::begin_under`: a subtransaction of a transaction started, its xid returned.
#[unsafe(no_mangle)]
pub extern "C" fn simc_begin_under(parent: ffi::HostXid) -> ffi::HostXid {
    entered!(ffi::HostXid { f0: sim::begin_under(Xid(parent.f0)).0 })
}

/// simc_commit is `sim::commit`: a transaction committed, with its subtransactions still in progress.
#[unsafe(no_mangle)]
pub extern "C" fn simc_commit(xid: ffi::HostXid) {
    entered!(sim::commit(Xid(xid.f0)))
}

/// simc_abort is `sim::abort`: a transaction or a subtransaction aborted, with its subtransactions still in progress.
#[unsafe(no_mangle)]
pub extern "C" fn simc_abort(xid: ffi::HostXid) {
    entered!(sim::abort(Xid(xid.f0)))
}

/// simc_truncate_commit_log is `sim::truncate_commit_log`: an xid becomes the oldest the commit log keeps.
#[unsafe(no_mangle)]
pub extern "C" fn simc_truncate_commit_log(oldest: ffi::HostXid) {
    entered!(sim::truncate_commit_log(Xid(oldest.f0)))
}

/// simc_savepoint is `sim::savepoint`: the running actor's transaction starts a subtransaction.
#[unsafe(no_mangle)]
pub extern "C" fn simc_savepoint() {
    entered!(sim::savepoint())
}

/// simc_rollback_to is `sim::rollback_to`: the running actor's current subtransaction aborts and is left.
#[unsafe(no_mangle)]
pub extern "C" fn simc_rollback_to() {
    entered!(sim::rollback_to())
}

/// simc_release is `sim::release`: the running actor leaves its current subtransaction, whose xid stays in progress.
#[unsafe(no_mangle)]
pub extern "C" fn simc_release() {
    entered!(sim::release())
}

/// simc_next_command is `sim::next_command`: the running actor's command ends, moving to the next where it was used.
#[unsafe(no_mangle)]
pub extern "C" fn simc_next_command() {
    entered!(sim::next_command())
}

/// simc_historic is `sim::historic`: a historic snapshot of its bounds, the xids that committed between them, the
/// decoded transaction's own xids, and the command the replay reached, which `simc_Taken_lent` lends.
///
/// # Safety
///
/// `committed` and `own` are `committed_len` and `own_len` xids for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_historic(
    xmin: ffi::HostXid,
    xmax: ffi::HostXid,
    committed: *const ffi::HostXid,
    committed_len: usize,
    own: *const ffi::HostXid,
    own_len: usize,
    command: ffi::HostCommand,
) -> ffi::SimcTaken {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids.
        let (committed, own) = unsafe { (c::xids(committed, committed_len), c::xids(own, own_len)) };
        let taken = sim::historic((Xid(xmin.f0), Xid(xmax.f0)), &committed, &own, Command(command.f0));
        ffi::SimcTaken { f0: handed(taken) }
    })
}

/// simc_snapshot is `sim::snapshot`: a snapshot of a kind of the simulation's running xids but the actor's own, in
/// recovery's shape when asked, which `simc_Taken_lent` lends.
#[unsafe(no_mangle)]
pub extern "C" fn simc_snapshot(kind: ffi::HostSnapshotKind, recovery: bool) -> ffi::SimcTaken {
    entered!({
        let kind = match kind {
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_MVCC => host::SnapshotKind::Mvcc,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_CURRENT => host::SnapshotKind::Current,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_ANY => host::SnapshotKind::Any,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_TOAST => host::SnapshotKind::Toast,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_DIRTY => host::SnapshotKind::Dirty,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_HISTORIC_MVCC => host::SnapshotKind::HistoricMvcc,
            ffi::HostSnapshotKind_HOST_SNAPSHOT_KIND_NON_VACUUMABLE => host::SnapshotKind::NonVacuumable,
            other => panic!("internal error: {other} is no snapshot kind"),
        };
        ffi::SimcTaken { f0: handed(sim::snapshot(kind, recovery)) }
    })
}

/// simc_non_vacuumable is `sim::non_vacuumable`: a NonVacuumable snapshot whose horizon is the caller's, which
/// `simc_Taken_lent` lends.
#[unsafe(no_mangle)]
pub extern "C" fn simc_non_vacuumable(horizon: ffi::HostXid) -> ffi::SimcTaken {
    entered!(ffi::SimcTaken { f0: handed(sim::non_vacuumable(Xid(horizon.f0))) })
}

/// simc_Taken_lent is `Taken::lent`: the snapshot lent for calls while the taken snapshot lives, its own snapshot the
/// taken one.
///
/// # Safety
///
/// `taken` is a snapshot `simc_historic` or `simc_snapshot` made and hasn't dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Taken_lent(taken: *const ffi::SimcTaken) -> ffi::HostSnapshot {
    entered!({
        // SAFETY: the caller's contract, a taken snapshot's box. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids.
        let held = unsafe { (*taken).f0 };
        // SAFETY: the box holds a `Taken` that outlives the lent snapshot, by the caller's contract. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids.
        c::snapshot(&unsafe { &*held.cast::<sim::Taken>() }.lent(), held)
    })
}

/// simc_Taken_drop is Taken's drop.
///
/// # Safety
///
/// `snapshot` is a snapshot `simc_historic` or `simc_snapshot` made, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Taken_drop(snapshot: *mut c_void) {
    // SAFETY: the caller's contract, a snapshot whose box holds a `Taken`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids.
    entered!(drop(unsafe { taken::<sim::Taken>((*snapshot.cast::<ffi::SimcTaken>()).f0) }))
}

/// simc_scratch_files is `sim::scratch_files`: how many scratch files are open, for a test of what a writer's drop
/// leaves.
#[unsafe(no_mangle)]
pub extern "C" fn simc_scratch_files() -> usize {
    entered!(sim::scratch_files())
}

/// simc_collecting is `sim::collecting`: the collection word, for a test that a failed collection ends what it began.
#[unsafe(no_mangle)]
pub extern "C" fn simc_collecting() -> u64 {
    entered!(sim::collecting())
}

/// simc_interrupts_held is `sim::interrupts_held`: how many holds of the process's interrupts are live, for a test
/// that a failure gives each back.
#[unsafe(no_mangle)]
pub extern "C" fn simc_interrupts_held() -> u64 {
    entered!(sim::interrupts_held())
}

/// simc_out_of_memory_at_next_decompress is `sim::out_of_memory_at_next_decompress`: the next decompression fails
/// with Postgres' out of memory error.
#[unsafe(no_mangle)]
pub extern "C" fn simc_out_of_memory_at_next_decompress() {
    entered!(sim::out_of_memory_at_next_decompress())
}

/// simc_fail_allocation fails the `nth` of simc's next allocations for the C, counting from 1, so that a test reaches
/// what a failed allocation does.
#[unsafe(no_mangle)]
pub extern "C" fn simc_fail_allocation(nth: u64) {
    entered!(c::fail_allocation(nth))
}

/// simc_connect is `sim::connect`: the running backend connected to a database, by its id, never 0, and the
/// tablespace of its own directory.
#[unsafe(no_mangle)]
pub extern "C" fn simc_connect(database: u32, tablespace: u32) {
    entered!(sim::connect(NonZeroU32::new(database).expect("a database's id is never 0"), tablespace))
}

/// sim_of reads an `Arc<Sim>` the C holds.
///
/// # Safety
///
/// `sim` is a simulation `simc_Sim_start` made, not yet dropped, which outlives `'a`.
unsafe fn sim_of<'a>(sim: *const ffi::SimcSim) -> &'a Arc<Sim> {
    // SAFETY: the caller's contract, a live simulation whose box holds an `Arc<Sim>`. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    unsafe { &*(*sim).f0.cast::<Arc<Sim>>() }
}

/// simc_Sim_start is `Sim::start`: the storage simulation over a directory's files, with a seed's choices.
///
/// # Safety
///
/// `root` and `root_len` are a path's bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_start(root: *const c_char, root_len: usize, seed: u64) -> ffi::SimcSim {
    // SAFETY: the caller's contract, a path's bytes for the call. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(ffi::SimcSim { f0: handed(Sim::start(unsafe { c::path(root, root_len) }, seed)) })
}

/// simc_Sim_stop is `Sim::stop`.
///
/// # Safety
///
/// `sim_of`'s contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_stop(sim: *const ffi::SimcSim) {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(unsafe { sim_of(sim) }.stop())
}

/// simc_Sim_trace is `Sim::trace`: the simulated calls' record, each a `String`.
///
/// # Safety
///
/// `sim_of`'s contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_trace(sim: *const ffi::SimcSim) -> ffi::StdVec {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(c::strings(&unsafe { sim_of(sim) }.trace()))
}

/// simc_Sim_calls is `Sim::calls`.
///
/// # Safety
///
/// `sim_of`'s contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_calls(sim: *const ffi::SimcSim) -> u64 {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(unsafe { sim_of(sim) }.calls())
}

/// simc_Sim_full_at is `Sim::full_at`.
///
/// # Safety
///
/// `sim_of`'s contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_full_at(sim: *const ffi::SimcSim, call: u64) {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(unsafe { sim_of(sim) }.full_at(call))
}

/// simc_Sim_crash_at is `Sim::crash_at`.
///
/// # Safety
///
/// `sim_of`'s contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_crash_at(sim: *const ffi::SimcSim, call: u64) {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs.
    entered!(unsafe { sim_of(sim) }.crash_at(call))
}

/// simc_Sim_recover is `Sim::recover`: the directory taken back to what a power loss at the crash leaves, or the
/// error doing it.
///
/// # Safety
///
/// `sim_of`'s contract, and `error` a place for the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_recover(sim: *const ffi::SimcSim, torn: bool, error: *mut ffi::StdIoError) -> bool {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs.
    entered!(match unsafe { sim_of(sim) }.recover(torn) {
        Ok(()) => true,
        Err(failure) => {
            // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs.
            unsafe { error.write(c::io_error(&failure)) };
            false
        }
    })
}

/// simc_Sim_drop is `Arc<Sim>`'s drop.
///
/// # Safety
///
/// `sim` is a simulation `simc_Sim_start` made, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Sim_drop(sim: *mut c_void) {
    // SAFETY: the caller's contract, a simulation whose box holds an `Arc<Sim>`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_simulation_counts_traces_and_fills_its_calls.
    entered!(drop(unsafe { taken::<Arc<Sim>>((*sim.cast::<ffi::SimcSim>()).f0) }))
}

/// simc_Run_new is `Run::new`: a run whose schedule a seed picks.
#[unsafe(no_mangle)]
pub extern "C" fn simc_Run_new(seed: u64) -> ffi::SimcRun {
    entered!(ffi::SimcRun { f0: handed(Run::new(seed)) })
}

/// Actor is an actor's C body, which simc runs on the actor's thread, or drops unrun with the run.
struct Actor {
    /// body is the C's boxed closure.
    body: ffi::SimcActor,
    /// called is whether the body was called, which consumed its captures.
    called: bool,
}

// SAFETY: an actor's body runs on one thread at a time, its run's handoffs ordering it with the rest; the C's closures
// over sim are written for that, as the Rust's `Send` bodies are. Tested by test/t/106_cport_simc.pl:
// simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
unsafe impl Send for Actor {}

impl Drop for Actor {
    /// drop drops the captures of a body that never ran, and frees a body's box.
    fn drop(&mut self) {
        // SAFETY: an unrun body's captures are its own, which its drop glue drops; the box is the C's, of `size` bytes
        // at `align`, freed once. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_run_dropped_unfinished_drops_its_actors_bodies.
        unsafe {
            if !self.called
                && let Some(drop_glue) = self.body.drop
            {
                drop_glue(self.body.captures);
            }
            if self.body.size != 0 {
                ffi::std_global_dealloc(self.body.captures, self.body.size, self.body.align);
            }
        }
    }
}

/// Called is an actor's call of its C body, through the C's catch of a panic.
struct Called {
    /// body is the C's closure.
    body: ffi::SimcActor,
    /// io is the actor's capability, the C's own.
    io: ffi::HostIoIo,
    /// error is the body's error, when it fails.
    error: MaybeUninit<ffi::HostError>,
    /// ok is whether the body returned without an error.
    ok: bool,
}

/// call calls an actor's C body inside the C's catch, holding nothing to drop, so a C panic in the body unwinds to the
/// catch past this frame.
///
/// # Safety
///
/// `called` is a `Called` the actor made for the call.
unsafe extern "C" fn call(called: *mut c_void) {
    // SAFETY: the caller's contract; the body's call takes its captures, the capability, and a place for its error.
    // Tested by test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    unsafe {
        let called = &mut *called.cast::<Called>();
        if let Some(call_once) = called.body.call_once {
            called.ok = call_once(called.body.captures, &mut called.io, called.error.as_mut_ptr());
        }
    }
}

impl Actor {
    /// run runs the body on the actor's thread with the backend's state an actor starts with, and hands sim a panic
    /// of the body's as its message alone, as sim's runner records an actor's panic.
    fn run(mut self) -> Result<(), host::Error> {
        self.called = true;
        // SAFETY: `simc_backend_fresh` reads the running thread's stack base and the backend's memory context, which
        // `simc_backend_restore` makes this actor's. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        unsafe { ffi::simc_backend_restore(&ffi::simc_backend_fresh()) };
        // SAFETY: `host_io_Io_testing` makes the C's capability around nothing. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        let io = unsafe { ffi::host_io_Io_testing() };
        let mut called = Called { body: self.body, io, error: MaybeUninit::uninit(), ok: false };
        let mut caught = MaybeUninit::<ffi::StdCaught>::uninit();
        // SAFETY: `std_catch_unwind` calls `call` with `called`, reports whether it returned, and writes the payload of
        // a panic it caught. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        let returned = unsafe { ffi::std_catch_unwind(Some(call), (&raw mut called).cast(), caught.as_mut_ptr()) };
        if !returned {
            // SAFETY: the catch wrote the caught payload, which simc reads and then drops. Tested by
            // test/t/106_cport_simc.pl: simc::tests::an_actors_c_panic_fails_its_run_with_its_message.
            let message = unsafe { c::caught_message(caught.as_mut_ptr()) };
            drop(self);
            std::panic::panic_any(message);
        }
        if called.ok {
            return Ok(());
        }
        // SAFETY: the body failed, so it wrote its error, which simc takes. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals.
        Err(unsafe { c::error_from(called.error.as_mut_ptr()) })
    }
}

/// simc_Run_actor is `Run::actor`: an actor added, which runs a C body with its own capability.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished; `name` and `name_len` are UTF-8 bytes; and `body` a boxed
/// closure the run takes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_actor(
    run: *mut ffi::SimcRun,
    name: *const c_char,
    name_len: usize,
    body: ffi::SimcActor,
) {
    entered!({
        let actor = Actor { body, called: false };
        // SAFETY: the caller's contract, a live run whose box holds a `Run`, and UTF-8 bytes. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        let (run, name) = unsafe { (&mut *(*run).f0.cast::<Run>(), c::slice::<u8>(name.cast(), name_len)) };
        let name = std::str::from_utf8(name).expect("an actor's name is UTF-8");
        run.actor(name, move |_| actor.run());
    })
}

/// simc_Run_finish is `Run::finish`: the actors run until each ended or the run crashed or failed, and the report.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, which this takes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_finish(run: ffi::SimcRun) -> ffi::SimcReport {
    entered!({
        // SAFETY: the caller's contract, a run whose box holds a `Run`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        let report = unsafe { taken::<Run>(run.f0) }.finish();
        ffi::SimcReport { outcome: c::outcome(&report.outcome), exits: c::exits(&report.exits), report: handed(report) }
    })
}

/// simc_Run_drop is Run's drop, of a run never finished: its actors' bodies dropped unrun.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not finished, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_drop(run: *mut c_void) {
    // SAFETY: the caller's contract, a run whose box holds a `Run`. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_run_dropped_unfinished_drops_its_actors_bodies.
    entered!(drop(unsafe { taken::<Run>((*run.cast::<ffi::SimcRun>()).f0) }))
}

/// simc_Report_drop is Report's drop: the C's outcome and exits freed, then the Rust's report, which releases the test
/// process's shared memory.
///
/// # Safety
///
/// `report` is a report `simc_Run_finish` returned, dropped once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Report_drop(report: *mut c_void) {
    entered!({
        // SAFETY: the caller's contract, a report whose outcome simc made and whose box holds a `Report`. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        unsafe {
            let report = &mut *report.cast::<ffi::SimcReport>();
            c::outcome_drop(&mut report.outcome);
            c::exits_drop(&mut report.exits);
            drop(taken::<sim::Report>(report.report));
        }
    })
}

/// simc_Report_Debug_fmt is Report's derived Debug, the Rust's own of the report the C holds.
///
/// # Safety
///
/// `value` is a report `simc_Run_finish` returned, not dropped, and `f` a formatter of the C's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Report_Debug_fmt(value: *const c_void, f: *mut ffi::StdFormatter) -> bool {
    entered!({
        // SAFETY: the caller's contract, a report whose box holds a `Report`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_reports_debug_is_the_rusts.
        unsafe {
            let report = &*(*value.cast::<ffi::SimcReport>()).report.cast::<sim::Report>();
            let text = match (*f).options.alternate {
                true => format!("{report:#?}"),
                false => format!("{report:?}"),
            };
            ffi::std_fmt_write_str(f, text.as_ptr().cast(), text.len())
        }
    })
}

/// simc_Report_trace is Report's `trace`, the actor at each yield point in order, as a C vector of `usize`s.
///
/// # Safety
///
/// `report` is a report `simc_Run_finish` returned, not dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Report_trace(report: *const ffi::SimcReport) -> ffi::StdVec {
    entered!({
        // SAFETY: the caller's contract, a report whose box holds a `Report`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_reports_trace_is_the_rusts.
        c::indexes(&unsafe { &*(*report).report.cast::<sim::Report>() }.trace)
    })
}

/// simc_Outcome_Debug_fmt is Outcome's derived Debug, the Rust's own of the outcome the C holds.
///
/// # Safety
///
/// `value` is an outcome `simc_Run_finish` made, and `f` the C's formatter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Outcome_Debug_fmt(value: *const c_void, f: *mut ffi::StdFormatter) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::an_actors_c_panic_fails_its_run_with_its_message.
        unsafe {
            let outcome = c::outcome_from(&*value.cast::<ffi::SimcOutcome>());
            let text = match (*f).options.alternate {
                true => format!("{outcome:#?}"),
                false => format!("{outcome:?}"),
            };
            ffi::std_fmt_write_str(f, text.as_ptr().cast(), text.len())
        }
    })
}

/// simc_Ending_Debug_fmt is Ending's derived Debug, the Rust's own of the ending the C holds, keeping of the C
/// formatter's options only `alternate`, the one the C's assertion messages use.
///
/// # Safety
///
/// `value` is an ending of the C's, and `f` the C's formatter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Ending_Debug_fmt(value: *const c_void, f: *mut ffi::StdFormatter) -> bool {
    entered!({
        // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
        unsafe {
            let ending = c::ending_from(&*value.cast::<ffi::SimcEnding>());
            let text = match (*f).options.alternate {
                true => format!("{ending:#?}"),
                false => format!("{ending:?}"),
            };
            ffi::std_fmt_write_str(f, text.as_ptr().cast(), text.len())
        }
    })
}

/// simc_Ending_PartialEq_eq is Ending's derived PartialEq.
///
/// # Safety
///
/// `this` and `other` are endings of the C's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Ending_PartialEq_eq(this: *const ffi::SimcEnding, other: *const ffi::SimcEnding) -> bool {
    // SAFETY: the caller's contract. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    entered!(unsafe { c::ending_from(&*this) == c::ending_from(&*other) })
}

/// run_and_name returns a live run and an actor's name of UTF-8 bytes.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished, and `name` and `name_len` are UTF-8 bytes, both for the
/// borrow.
unsafe fn run_and_name<'a>(run: *mut ffi::SimcRun, name: *const c_char, name_len: usize) -> (&'a mut Run, &'a str) {
    // SAFETY: the caller's contract, a live run whose box holds a `Run`, and UTF-8 bytes. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one.
    let (run, name) = unsafe { (&mut *(*run).f0.cast::<Run>(), c::slice::<u8>(name.cast(), name_len)) };
    (run, std::str::from_utf8(name).expect("an actor's name is UTF-8"))
}

/// simc_Run_until is `Run::until`: a step of the run's script that runs an actor until it pauses at an injection point.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished; `name` and `name_len` are UTF-8 bytes; and `point` is a
/// NUL-terminated string literal, which lives as long as the run.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_until(
    run: *mut ffi::SimcRun,
    name: *const c_char,
    name_len: usize,
    point: *const c_char,
) {
    entered!({
        // SAFETY: the caller's contract, a live run, a name, and a string literal, which lives for the whole program.
        // Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one.
        let (run, name, point) = unsafe {
            let (run, name) = run_and_name(run, name, name_len);
            (run, name, CStr::from_ptr(point))
        };
        run.until(name, point);
    })
}

/// simc_Run_script is `Run::script`: steps of the run's script, each running an actor for its count of yields.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished, and `steps` and `steps_len` are steps whose names are UTF-8
/// bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_script(run: *mut ffi::SimcRun, steps: *const ffi::SimcStep, steps_len: usize) {
    entered!({
        // SAFETY: the caller's contract, a live run and steps of UTF-8 names. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one.
        let (run, steps) = unsafe {
            let steps = std::slice::from_raw_parts(steps, steps_len);
            let named: Vec<(&str, u64)> = steps
                .iter()
                .map(|step| {
                    let name = c::slice::<u8>(step.name.cast(), step.name_len);
                    (std::str::from_utf8(name).expect("an actor's name is UTF-8"), step.yields)
                })
                .collect();
            (&mut *(*run).f0.cast::<Run>(), named)
        };
        run.script(&steps);
    })
}

/// simc_Run_terminate_at is `Run::terminate_at`: an actor terminated once it has yielded a count of times.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished, and `name` and `name_len` are UTF-8 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_terminate_at(
    run: *mut ffi::SimcRun,
    name: *const c_char,
    name_len: usize,
    yields: u64,
) {
    entered!({
        // SAFETY: the caller's contract, a live run and a name. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one.
        let (run, name) = unsafe { run_and_name(run, name, name_len) };
        run.terminate_at(name, yields);
    })
}

/// Acting is a C body `simc_as_actor` runs, through the C's catch of a panic.
struct Acting {
    /// body is the C's function.
    body: unsafe extern "C" fn(*mut c_void),
    /// captures is its argument.
    captures: *mut c_void,
}

/// act calls a C body inside the C's catch, holding nothing to drop, so a C panic in the body unwinds to the catch
/// past this frame.
///
/// # Safety
///
/// `acting` is an `Acting` `simc_as_actor` made for the call.
unsafe extern "C" fn act(acting: *mut c_void) {
    // SAFETY: the caller's contract; the body takes its captures. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory.
    unsafe {
        let acting = &*acting.cast::<Acting>();
        (acting.body)(acting.captures);
    }
}

/// simc_as_actor is `sim::as_actor`: a C body run on the calling thread as the actor of an id below the ids a run's
/// actors take, so its backend-local values are that actor's; a C panic in the body is raised again in the C.
///
/// # Safety
///
/// `body` is a C function that takes `captures`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_as_actor(actor: u64, body: unsafe extern "C" fn(*mut c_void), captures: *mut c_void) {
    entered!({
        let mut acting = Acting { body, captures };
        let mut caught = MaybeUninit::<ffi::StdCaught>::uninit();
        // SAFETY: `std_catch_unwind` calls `act` with `acting`, reports whether it returned, and writes the payload of
        // a
        // panic it caught. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory.
        let returned = sim::as_actor(actor, || unsafe {
            ffi::std_catch_unwind(Some(act), (&raw mut acting).cast(), caught.as_mut_ptr())
        });
        if !returned {
            // SAFETY: the catch wrote the caught payload, which the entry raises in the C unchanged. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory.
            std::panic::resume_unwind(Box::new(crate::entry::Caught(unsafe { caught.assume_init() })));
        }
    })
}

/// simc_checkpointed is `sim::checkpointed`: the redo point `host::last_redo` answers from now on.
#[unsafe(no_mangle)]
pub extern "C" fn simc_checkpointed(redo: ffi::HostLsn) {
    entered!(sim::checkpointed(redo))
}

/// simc_data_dir is `sim::data_dir`: the simulated server's data directory, one for the test process.
#[unsafe(no_mangle)]
pub extern "C" fn simc_data_dir() -> ffi::StdPathBuf {
    entered!(c::path_buf(&sim::data_dir()))
}

/// simc_take_warnings is `sim::take_warnings`: the reports at WARNING the work made since they were last taken, a
/// C vector of each one's SQLSTATE and message.
#[unsafe(no_mangle)]
pub extern "C" fn simc_take_warnings() -> ffi::StdVec {
    entered!(c::warnings(&sim::take_warnings()))
}

/// simc_take_logs is `sim::take_logs`: the reports at LOG the work made since they were last taken.
#[unsafe(no_mangle)]
pub extern "C" fn simc_take_logs() -> ffi::StdVec {
    entered!(c::strings(&sim::take_logs()))
}

/// simc_kept_wal_read is `sim::kept_wal_read`: whether a check read the WAL kept for a fallback since it was last
/// taken, and the range it read.
#[unsafe(no_mangle)]
pub extern "C" fn simc_kept_wal_read(from: &mut ffi::HostLsn, to: &mut ffi::HostLsn) -> bool {
    entered!(match sim::kept_wal_read() {
        Some((first, last)) => {
            (*from, *to) = (first, last);
            true
        }
        None => false,
    })
}

/// simc_logged is `sim::logged`: every record the simulation's WAL took, its kind's value, its body, and its end.
#[unsafe(no_mangle)]
pub extern "C" fn simc_logged() -> ffi::StdVec {
    entered!(c::logged(&sim::logged()))
}

/// simc_new_cids is `sim::new_cids`: every new cid record the simulation's WAL took, its storage, its row, its
/// command ids, and its end.
#[unsafe(no_mangle)]
pub extern "C" fn simc_new_cids() -> ffi::StdVec {
    entered!(c::new_cids(&sim::new_cids()))
}

/// simc_size_delta is `sim::size_delta`: the delta started afresh with a budget, a test's `delta_memory`.
#[unsafe(no_mangle)]
pub extern "C" fn simc_size_delta(budget: u64) {
    entered!(sim::size_delta(budget))
}

/// simc_delta_holds is `sim::delta_holds`: the count of holds the delta has begun.
#[unsafe(no_mangle)]
pub extern "C" fn simc_delta_holds() -> u64 {
    entered!(sim::delta_holds())
}

/// simc_free_slots is `sim::free_slots`: the count of the delta directory's slots no page holds.
#[unsafe(no_mangle)]
pub extern "C" fn simc_free_slots() -> usize {
    entered!(sim::free_slots())
}

/// simc_delta_waiting is `sim::delta_waiting`: the count of processes asleep at the delta's budget.
#[unsafe(no_mangle)]
pub extern "C" fn simc_delta_waiting() -> u64 {
    entered!(sim::delta_waiting())
}

/// simc_delta_wakes is `sim::delta_wakes`: the count of frees a hold's end counted to wake the budget's waiters.
#[unsafe(no_mangle)]
pub extern "C" fn simc_delta_wakes() -> u64 {
    entered!(sim::delta_wakes())
}

/// simc_unfreed is `sim::unfreed`: how many records of an aged kind, by its shape's id, and how many pages of the
/// directory no free pass has freed.
#[unsafe(no_mangle)]
pub extern "C" fn simc_unfreed(shape: u64, records: &mut usize, pages: &mut usize) {
    entered!((*records, *pages) = sim::unfreed(shape))
}

/// simc_size_shared is `sim::size_shared`: the shared node cache emptied and given a number of frames in each class.
#[unsafe(no_mangle)]
pub extern "C" fn simc_size_shared(counts: &[usize; host::cache::CLASSES.len()]) {
    entered!(sim::size_shared(counts))
}

/// simc_end is `sim::end`: the running actor's transaction committed or aborted, and a fresh one started.
#[unsafe(no_mangle)]
pub extern "C" fn simc_end(committed: bool) {
    entered!(sim::end(committed))
}

/// simc_unanswered is `sim::unanswered`: the running thread made to stand for a process in crash recovery or for one
/// out of it.
#[unsafe(no_mangle)]
pub extern "C" fn simc_unanswered(recovering: bool) {
    entered!(sim::unanswered(recovering))
}

/// simc_delta_passes is `sim::delta_passes`: the count of free passes the delta has run.
#[unsafe(no_mangle)]
pub extern "C" fn simc_delta_passes() -> u64 {
    entered!(sim::delta_passes())
}

/// CHECK is the C check the running test's run was given, which `checked` calls at the run's yield points.
static CHECK: Mutex<Option<unsafe extern "C" fn(*mut ffi::StdString) -> bool>> = Mutex::new(None);

/// Checking is a run's call of its C check, through the C's catch of a panic.
struct Checking {
    /// check is the C's check.
    check: unsafe extern "C" fn(*mut ffi::StdString) -> bool,
    /// message is the check's message, when it fails.
    message: MaybeUninit<ffi::StdString>,
    /// failed is whether the check failed.
    failed: bool,
}

/// check_once calls a run's C check inside the C's catch, holding nothing to drop, so a C panic in the check unwinds
/// to the catch past this frame.
///
/// # Safety
///
/// `checking` is a `Checking` `checked` made for the call.
unsafe extern "C" fn check_once(checking: *mut c_void) {
    // SAFETY: the caller's contract; the check takes a place for its message. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message.
    unsafe {
        let checking = &mut *checking.cast::<Checking>();
        checking.failed = (checking.check)(checking.message.as_mut_ptr());
    }
}

/// checked runs the C check `simc_Run_check` gave the run, raising a C panic in it as its message, and returns the
/// message it writes when it fails, copied out of the C's string, which it frees.
fn checked() -> Option<String> {
    let check = (*CHECK.lock().unwrap_or_else(PoisonError::into_inner))?;
    let mut checking = Checking { check, message: MaybeUninit::uninit(), failed: false };
    let mut caught = MaybeUninit::<ffi::StdCaught>::uninit();
    // SAFETY: `std_catch_unwind` calls `check_once` with `checking`, reports whether it returned, and writes the
    // payload of a panic it caught. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message.
    let returned = unsafe { ffi::std_catch_unwind(Some(check_once), (&raw mut checking).cast(), caught.as_mut_ptr()) };
    if !returned {
        // SAFETY: the catch wrote the caught payload, which simc reads and then drops. Tested by
        // test/t/106_cport_simc.pl: simc::tests::a_run_checks_c_panic_fails_the_run_with_its_message.
        std::panic::panic_any(unsafe { c::caught_message(caught.as_mut_ptr()) });
    }
    if !checking.failed {
        return None;
    }
    // SAFETY: a failed check wrote its message, a C `String` of UTF-8 bytes from the global allocator at alignment 1,
    // which simc copies and then frees. Tested by test/t/106_cport_simc.pl:
    // simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message.
    unsafe {
        let message = checking.message.assume_init();
        let text = String::from_utf8_lossy(c::slice(message.ptr.cast::<u8>(), message.len)).into_owned();
        if message.cap != 0 {
            ffi::std_global_dealloc(message.ptr, message.cap, 1);
        }
        Some(text)
    }
}

/// simc_Run_check is `Run::check`: a C check run at every yield point of the run's actors, which fails the run there
/// with the message it writes when it answers true.
///
/// # Safety
///
/// `run` is a run `simc_Run_new` made, not yet finished, and `check` a C function that writes a `String` of UTF-8
/// bytes when it answers true.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn simc_Run_check(
    run: *mut ffi::SimcRun,
    check: unsafe extern "C" fn(*mut ffi::StdString) -> bool,
) {
    entered!({
        *CHECK.lock().unwrap_or_else(PoisonError::into_inner) = Some(check);
        // SAFETY: the caller's contract, a live run whose box holds a `Run`. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message.
        let run = unsafe { &mut *(*run).f0.cast::<Run>() };
        run.check(checked);
    })
}
