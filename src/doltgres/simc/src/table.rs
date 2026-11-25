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

//! The simulation's host as the C port's host table (the port text's sections 8 and 13): each entry the C's doors
//! call converts its arguments and calls the simulation's host directly, since the C's doors already ran the door's
//! own work, under `entry::enter`, and writes the answer to the C's places.

use std::ffi::{CStr, c_char, c_void};

use host::delta::Leaving;
use host::{Error, Host, MayRaise, Open, ReadHost, Xid};
use sim::SimHost;

use crate::c;
use crate::entry::entered;
use crate::ffi;

/// answered writes a seam's answer to the C's places, the value or the error, and returns whether it succeeded.
///
/// # Safety
///
/// `result` and `error` are places the C passed for the call's answer.
unsafe fn answered<T>(answer: Result<T, Error>, result: *mut T, error: *mut ffi::HostError) -> bool {
    match answer {
        // SAFETY: the caller's contract, a place for the value. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        Ok(value) => unsafe { result.write(value) },
        // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        Err(failure) => return unsafe { failed(Err(failure), error) },
    }
    true
}

/// failed writes a seam's error to the C's place, and returns whether the seam succeeded.
///
/// # Safety
///
/// `error` is a place the C passed for the call's error.
unsafe fn failed(answer: Result<(), Error>, error: *mut ffi::HostError) -> bool {
    let Err(failure) = answer else { return true };
    let made = c::error(failure);
    // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
    // simc::tests::each_served_entry_answers_as_the_simulations_host.
    unsafe { error.write(made) };
    false
}

/// maybe writes a seam's optional answer to the C's places, whether it has one and the value, or the error.
///
/// # Safety
///
/// `has_result`, `result`, and `error` are places the C passed for the call's answer.
unsafe fn maybe<T>(
    answer: Result<Option<T>, Error>,
    has_result: *mut bool,
    result: *mut T,
    error: *mut ffi::HostError,
) -> bool {
    match answer {
        Ok(found) => {
            // SAFETY: the caller's contract, places for the answer's presence and its value. Tested by
            // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                has_result.write(found.is_some());
                if let Some(value) = found {
                    result.write(value);
                }
            }
            true
        }
        // SAFETY: the caller's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        Err(failure) => unsafe { failed(Err(failure), error) },
    }
}

/// ReadHost's entries: `ReadHost`'s methods over the simulation's host. Each one's SAFETY is its C door's contract:
/// the paths and bytes it passes live for the call, and its places are writable.
mod read {
    use super::*;

    /// open_to_read is `ReadHost::open_to_read`.
    pub(super) unsafe extern "C" fn open_to_read(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { answered(SimHost.open_to_read(&MayRaise::testing(), c::path(path, path_len)), result, error) }
        })
    }

    /// open_if_there is `ReadHost::open_if_there`.
    pub(super) unsafe extern "C" fn open_if_there(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        has_result: *mut bool,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.open_if_there(&MayRaise::testing(), c::path(path, path_len));
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// read_at is `ReadHost::read_at`.
    pub(super) unsafe extern "C" fn read_at(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        file: i32,
        offset: u64,
        buffer: *mut u8,
        buffer_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.read_at(&MayRaise::testing(), file, offset, c::bytes_mut(buffer, buffer_len));
                failed(answer, error)
            }
        })
    }

    /// size is `ReadHost::size`.
    pub(super) unsafe extern "C" fn size(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        file: i32,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { answered(SimHost.size(&MayRaise::testing(), file), result, error) })
    }

    /// prefetch is `ReadHost::prefetch`.
    pub(super) unsafe extern "C" fn prefetch(_: *mut c_void, file: i32, offset: u64, len: u64) {
        entered!(SimHost.prefetch(file, offset, len))
    }

    /// close is `ReadHost::close`.
    pub(super) unsafe extern "C" fn close(_: *mut c_void, file: i32) {
        entered!(SimHost.close(file))
    }

    /// tablespace_root is `ReadHost::tablespace_root`.
    pub(super) unsafe extern "C" fn tablespace_root(_: *mut c_void, tablespace: u32) -> ffi::StdPathBuf {
        entered!(c::path_buf(&SimHost.tablespace_root(tablespace)))
    }

    /// list_dir is `ReadHost::list_dir`.
    pub(super) unsafe extern "C" fn list_dir(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.list_dir(&MayRaise::testing(), c::path(path, path_len));
                answered(answer.map(|names| c::strings(&names)), result, error)
            }
        })
    }

    /// list_dir_if_there is `ReadHost::list_dir_if_there`.
    pub(super) unsafe extern "C" fn list_dir_if_there(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        has_result: *mut bool,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.list_dir_if_there(&MayRaise::testing(), c::path(path, path_len));
                maybe(answer.map(|names| names.map(|names| c::strings(&names))), has_result, result, error)
            }
        })
    }

    /// prepare_decompressing is `ReadHost::prepare_decompressing`.
    pub(super) unsafe extern "C" fn prepare_decompressing(
        _: *mut c_void,
        bytes: *const u8,
        bytes_len: usize,
        result: *mut ffi::HostDecompressDictionary,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            let answer = SimHost.prepare_decompressing(unsafe { c::slice(bytes, bytes_len) });
            let made = answer
                .map(|prepared| ffi::HostDecompressDictionary { f0: c::any(prepared, &c::DECOMPRESS_DICTIONARY) });
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            unsafe { answered(made, result, error) }
        })
    }

    /// frame_size is `ReadHost::frame_size`.
    pub(super) unsafe extern "C" fn frame_size(
        _: *mut c_void,
        frame: *const u8,
        frame_len: usize,
        result: *mut u64,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
        entered!(match SimHost.frame_size(unsafe { c::slice(frame, frame_len) }) {
            None => false,
            Some(size) => {
                // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
                // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
                unsafe { result.write(size) };
                true
            }
        })
    }

    /// decompress is `ReadHost::decompress`, over a dictionary simc prepared when the C passes one.
    pub(super) unsafe extern "C" fn decompress(
        _: *mut c_void,
        frame: *const u8,
        frame_len: usize,
        size: usize,
        dictionary: *const ffi::HostDecompressDictionary,
        has_result: *mut bool,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract: the frame's bytes, and a dictionary simc prepared, or none. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            let (frame, dictionary) = unsafe {
                let dictionary = dictionary.as_ref().map(|held| c::any_ref(&held.f0, &c::DECOMPRESS_DICTIONARY));
                (c::slice(frame, frame_len), dictionary)
            };
            let answer = SimHost.decompress(frame, size, dictionary);
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            unsafe { maybe(answer.map(|bytes| bytes.map(|bytes| c::vec(&bytes))), has_result, result, error) }
        })
    }
}

/// Host's entries: `Host`'s methods over the simulation's host, each SAFETY its C door's contract, as `read`'s are.
mod write {
    use super::*;

    use std::any::TypeId;
    use std::sync::{Mutex, PoisonError};

    /// CRun is a C exit work's function, which takes the C's capability and a place for its error.
    type CRun = unsafe extern "C" fn(*mut ffi::HostIoIo, *mut ffi::HostError) -> bool;

    /// Run is a sim exit work's function.
    type Run = fn(&mut host::io::Io) -> Result<(), Error>;

    /// EXIT_SLOTS is how many kinds of C exit work simc serves, more than the crates' kinds of exit work.
    const EXIT_SLOTS: usize = 16;

    /// C_WORKS are the kinds of C exit work in the order simc first saw each, the address of the type that names it
    /// and its function, each kind's place its slot.
    static C_WORKS: Mutex<Vec<(usize, CRun)>> = Mutex::new(Vec::new());

    /// CExit names the sim exit work of a slot.
    struct CExit<const N: usize>;

    /// c_exit is a slot's sim exit work, which runs the slot's C exit work, so that sim runs each C work as its own.
    fn c_exit<const N: usize>(_: &mut host::io::Io) -> Result<(), Error> {
        let run = C_WORKS.lock().unwrap_or_else(PoisonError::into_inner)[N].1;
        // SAFETY: `host_io_Io_testing` makes the C's capability around nothing, and the C's exit work takes it and a
        // place for its error, which it writes when it fails. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_failing_exit_work_is_in_the_report.
        let failed = unsafe {
            let mut io = ffi::host_io_Io_testing();
            let mut error = std::mem::MaybeUninit::<ffi::HostError>::uninit();
            (!run(&mut io, error.as_mut_ptr())).then(|| c::error_from(error.as_mut_ptr()))
        };
        failed.map_or(Ok(()), Err)
    }

    /// exit_slots makes the table of the slots' sim exit works, each the type that names it and its function.
    macro_rules! exit_slots {
        ($($n:literal)*) => { [$((TypeId::of::<CExit<$n>> as fn() -> TypeId, c_exit::<$n> as Run)),*] };
    }

    /// EXITS are the slots' sim exit works, slot N's at N.
    static EXITS: [Exit; EXIT_SLOTS] = exit_slots!(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15);

    /// Exit is a slot's sim exit work: the type that names it, and its function.
    type Exit = (fn() -> TypeId, Run);

    /// at_exit is `Host::at_exit`: the C's work listed with sim as its slot's own exit work, which sim lists once for
    /// the calling process, runs at its exit newest first with the process's flags cleared before it, reports should
    /// it fail, and refuses once the process's exit has begun.
    pub(super) unsafe extern "C" fn at_exit(
        _: *mut c_void,
        work: *const ffi::StdTypeId,
        run: ::core::option::Option<CRun>,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            let run = run.expect("an exit work has a function");
            let slot = {
                let mut works = C_WORKS.lock().unwrap_or_else(PoisonError::into_inner);
                match works.iter().position(|&(listed, _)| listed == work as usize) {
                    Some(slot) => Ok(slot),
                    None if works.len() < EXIT_SLOTS => {
                        works.push((work as usize, run));
                        Ok(works.len() - 1)
                    }
                    None => {
                        Err(Error::fork(b"XX000", format!("simc serves at most {EXIT_SLOTS} kinds of C exit work")))
                    }
                }
            };
            let listed = slot.and_then(|slot| {
                let (named, run) = EXITS[slot];
                SimHost.at_exit(named(), run)
            });
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::an_exit_work_listed_once_the_exit_began_is_refused.
            unsafe { failed(listed, error) }
        })
    }

    /// shared_make is `Host::shared_make`.
    pub(super) unsafe extern "C" fn shared_make(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        has_result: *mut bool,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.shared_make(&MayRaise::testing(), &kind));
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// shared_open is `Host::shared_open`.
    pub(super) unsafe extern "C" fn shared_open(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        location: u64,
        has_result: *mut bool,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
            unsafe {
                let answer =
                    c::kind(&*kind).and_then(|kind| SimHost.shared_open(&MayRaise::testing(), &kind, location));
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// flag_take is `Host::flag_take`.
    pub(super) unsafe extern "C" fn flag_take(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        place: u64,
        flag: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_processs_flags_are_cleared_before_each_exit_work.
            unsafe { failed(SimHost.flag_take(&MayRaise::testing(), place, flag), error) }
        })
    }

    /// flag_try is `Host::flag_try`.
    pub(super) unsafe extern "C" fn flag_try(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        place: u64,
        flag: usize,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer and the error. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_flag_try_takes_a_free_flag_and_refuses_its_own.
            unsafe { answered(SimHost.flag_try(&MayRaise::testing(), place, flag), result, error) }
        })
    }

    /// flag_clear is `Host::flag_clear`.
    pub(super) unsafe extern "C" fn flag_clear(_: *mut c_void, place: u64, flag: usize) {
        entered!(SimHost.flag_clear(place, flag))
    }

    /// proc_number is `Host::proc_number`: the running process's number among the simulated server's.
    pub(super) unsafe extern "C" fn proc_number(_: *mut c_void) -> u32 {
        entered!(SimHost.proc_number())
    }

    /// now is `Host::now`.
    pub(super) unsafe extern "C" fn now(_: *mut c_void) -> ffi::StdSystemTime {
        entered!(c::system_time(SimHost.now()))
    }

    /// instant is `Host::instant`.
    pub(super) unsafe extern "C" fn instant(_: *mut c_void) -> ffi::StdInstant {
        entered!(c::instant(SimHost.instant()))
    }

    /// process_id is `Host::process_id`.
    pub(super) unsafe extern "C" fn process_id(_: *mut c_void) -> u32 {
        entered!(SimHost.process_id())
    }

    /// work_mem is `Host::work_mem`, the simulation's fixed 4 MB.
    pub(super) unsafe extern "C" fn work_mem(_: *mut c_void) -> u64 {
        entered!(SimHost.work_mem())
    }

    /// session_database is `Host::session_database`.
    pub(super) unsafe extern "C" fn session_database(_: *mut c_void, out_0: *mut u32, out_1: *mut u32) -> bool {
        entered!(match SimHost.session_database() {
            None => false,
            Some((database, tablespace)) => {
                // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
                // simc::tests::each_served_entry_answers_as_the_simulations_host.
                unsafe {
                    out_0.write(database.get());
                    out_1.write(tablespace);
                }
                true
            }
        })
    }

    /// exiting is `Host::exiting`.
    pub(super) unsafe extern "C" fn exiting(_: *mut c_void) -> bool {
        entered!(SimHost.exiting())
    }

    /// backups_running is `Host::backups_running`.
    pub(super) unsafe extern "C" fn backups_running(_: *mut c_void) -> bool {
        entered!(SimHost.backups_running())
    }

    /// slots_name is `Host::slots_name`.
    pub(super) unsafe extern "C" fn slots_name(_: *mut c_void, place: u64, array: usize) -> u64 {
        entered!(SimHost.slots_name(place, array))
    }

    /// slots_rename is `Host::slots_rename`.
    pub(super) unsafe extern "C" fn slots_rename(
        _: *mut c_void,
        place: u64,
        array: usize,
        loaded: u64,
        name: u64,
    ) -> bool {
        entered!(SimHost.slots_rename(place, array, loaded, name))
    }

    /// slots_empty is `Host::slots_empty`.
    pub(super) unsafe extern "C" fn slots_empty(_: *mut c_void, place: u64, array: usize) {
        entered!(SimHost.slots_empty(place, array))
    }

    /// slots_claim is `Host::slots_claim`.
    pub(super) unsafe extern "C" fn slots_claim(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        place: u64,
        array: usize,
        bytes: *const u8,
        bytes_len: usize,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_records_slots_answer_as_the_simulations_host.
            unsafe {
                let claimed = SimHost.slots_claim(&MayRaise::testing(), place, array, c::slice(bytes, bytes_len));
                answered(claimed, result, error)
            }
        })
    }

    /// slots_read is `Host::slots_read`.
    pub(super) unsafe extern "C" fn slots_read(_: *mut c_void, place: u64, array: usize) -> ffi::StdVec {
        entered!(c::vecs(&SimHost.slots_read(place, array)))
    }

    /// unique is `Host::unique`.
    pub(super) unsafe extern "C" fn unique(_: *mut c_void) -> u64 {
        entered!(SimHost.unique())
    }

    /// check_interrupts is `Host::check_interrupts`.
    pub(super) unsafe extern "C" fn check_interrupts(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { failed(SimHost.check_interrupts(&MayRaise::testing()), error) })
    }

    /// hold_interrupts is `Host::hold_interrupts`.
    pub(super) unsafe extern "C" fn hold_interrupts(_: *mut c_void) {
        entered!(SimHost.hold_interrupts())
    }

    /// resume_interrupts is `Host::resume_interrupts`.
    pub(super) unsafe extern "C" fn resume_interrupts(_: *mut c_void) {
        entered!(SimHost.resume_interrupts())
    }

    /// open is `Host::open`.
    pub(super) unsafe extern "C" fn open(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        how: ffi::HostOpen,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        let how = match how {
            ffi::HostOpen_HOST_OPEN_READ_WRITE => Open::ReadWrite,
            _ => Open::CreateNew,
        };
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { answered(SimHost.open(&MayRaise::testing(), c::path(path, path_len), how), result, error) }
        })
    }

    /// create_if_absent is `Host::create_if_absent`.
    pub(super) unsafe extern "C" fn create_if_absent(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        has_result: *mut bool,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.create_if_absent(&MayRaise::testing(), c::path(path, path_len));
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// open_scratch is `Host::open_scratch`.
    pub(super) unsafe extern "C" fn open_scratch(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { answered(SimHost.open_scratch(&MayRaise::testing()), result, error) })
    }

    /// write_at is `Host::write_at`.
    pub(super) unsafe extern "C" fn write_at(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        file: i32,
        offset: u64,
        bytes: *const u8,
        bytes_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.write_at(&MayRaise::testing(), file, offset, c::slice(bytes, bytes_len));
                failed(answer, error)
            }
        })
    }

    /// truncate is `Host::truncate`.
    pub(super) unsafe extern "C" fn truncate(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        file: i32,
        len: u64,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { failed(SimHost.truncate(&MayRaise::testing(), file, len), error) })
    }

    /// sync is `Host::sync`.
    pub(super) unsafe extern "C" fn sync(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        file: i32,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { failed(SimHost.sync(&MayRaise::testing(), file), error) })
    }

    /// rename is `Host::rename`.
    pub(super) unsafe extern "C" fn rename(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        from: *const c_char,
        from_len: usize,
        to: *const c_char,
        to_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.rename(&MayRaise::testing(), c::path(from, from_len), c::path(to, to_len));
                failed(answer, error)
            }
        })
    }

    /// rename_durably is `Host::rename_durably`.
    pub(super) unsafe extern "C" fn rename_durably(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        from: *const c_char,
        from_len: usize,
        to: *const c_char,
        to_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let (from, to) = (c::path(from, from_len), c::path(to, to_len));
                failed(SimHost.rename_durably(&MayRaise::testing(), from, to), error)
            }
        })
    }

    /// remove_durably is `Host::remove_durably`.
    pub(super) unsafe extern "C" fn remove_durably(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { failed(SimHost.remove_durably(&MayRaise::testing(), c::path(path, path_len)), error) }
        })
    }

    /// remove_durably_if_there is `Host::remove_durably_if_there`.
    pub(super) unsafe extern "C" fn remove_durably_if_there(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let answer = SimHost.remove_durably_if_there(&MayRaise::testing(), c::path(path, path_len));
                answered(answer, result, error)
            }
        })
    }

    /// remove_leftover is `Host::remove_leftover`.
    pub(super) unsafe extern "C" fn remove_leftover(_: *mut c_void, path: *const c_char, path_len: usize) {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { SimHost.remove_leftover(c::path(path, path_len)) })
    }

    /// rename_back is `Host::rename_back`.
    pub(super) unsafe extern "C" fn rename_back(
        _: *mut c_void,
        from: *const c_char,
        from_len: usize,
        to: *const c_char,
        to_len: usize,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { SimHost.rename_back(c::path(from, from_len), c::path(to, to_len)) })
    }

    /// remove_tree is `Host::remove_tree`.
    pub(super) unsafe extern "C" fn remove_tree(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { answered(SimHost.remove_tree(&MayRaise::testing(), c::path(path, path_len)), result, error) }
        })
    }

    /// sync_path is `Host::sync_path`.
    pub(super) unsafe extern "C" fn sync_path(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        directory: bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { failed(SimHost.sync_path(&MayRaise::testing(), c::path(path, path_len), directory), error) }
        })
    }

    /// make_dir is `Host::make_dir`.
    pub(super) unsafe extern "C" fn make_dir(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { failed(SimHost.make_dir(&MayRaise::testing(), c::path(path, path_len)), error) }
        })
    }

    /// disk_space is `Host::disk_space`.
    pub(super) unsafe extern "C" fn disk_space(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result_0: *mut u64,
        result_1: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe {
                let mut both = (0, 0);
                let ok =
                    answered(SimHost.disk_space(&MayRaise::testing(), c::path(path, path_len)), &raw mut both, error);
                if ok {
                    result_0.write(both.0);
                    result_1.write(both.1);
                }
                ok
            }
        })
    }

    /// tree_bytes is `Host::tree_bytes`.
    pub(super) unsafe extern "C" fn tree_bytes(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        path: *const c_char,
        path_len: usize,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::each_served_entry_answers_as_the_simulations_host.
            unsafe { answered(SimHost.tree_bytes(&MayRaise::testing(), c::path(path, path_len)), result, error) }
        })
    }

    /// nap is `Host::nap`.
    pub(super) unsafe extern "C" fn nap(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        duration: ffi::StdDuration,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::each_served_entry_answers_as_the_simulations_host.
        entered!(unsafe { failed(SimHost.nap(&MayRaise::testing(), c::duration_from(duration)), error) })
    }

    /// actor is `Host::actor`.
    pub(super) unsafe extern "C" fn actor(_: *mut c_void, result: *mut u64) -> bool {
        entered!(match SimHost.actor() {
            None => false,
            Some(actor) => {
                // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
                // simc::tests::each_served_entry_answers_as_the_simulations_host.
                unsafe { result.write(actor) };
                true
            }
        })
    }

    /// yield_point is `Host::yield_point`.
    pub(super) unsafe extern "C" fn yield_point(_: *mut c_void) {
        entered!(SimHost.yield_point())
    }

    /// prepare_compressing is `Host::prepare_compressing`.
    pub(super) unsafe extern "C" fn prepare_compressing(
        _: *mut c_void,
        bytes: *const u8,
        bytes_len: usize,
        level: i32,
        result: *mut ffi::HostCompressDictionary,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            let answer = SimHost.prepare_compressing(unsafe { c::slice(bytes, bytes_len) }, level);
            let made =
                answer.map(|prepared| ffi::HostCompressDictionary { f0: c::any(prepared, &c::COMPRESS_DICTIONARY) });
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            unsafe { answered(made, result, error) }
        })
    }

    /// compress is `Host::compress`, over a dictionary simc prepared when the C passes one.
    pub(super) unsafe extern "C" fn compress(
        _: *mut c_void,
        bytes: *const u8,
        bytes_len: usize,
        level: i32,
        dictionary: *const ffi::HostCompressDictionary,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract: the bytes, and a dictionary simc prepared, or none. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            let (bytes, dictionary) = unsafe {
                let dictionary = dictionary.as_ref().map(|held| c::any_ref(&held.f0, &c::COMPRESS_DICTIONARY));
                (c::slice(bytes, bytes_len), dictionary)
            };
            let answer = SimHost.compress(bytes, level, dictionary);
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c.
            unsafe { answered(answer.map(|frame| c::vec(&frame)), result, error) }
        })
    }

    /// train is `Host::train`, over the C's samples.
    pub(super) unsafe extern "C" fn train(
        _: *mut c_void,
        samples: *const ffi::StdBytes,
        samples_len: usize,
        capacity: usize,
        result: *mut ffi::StdVec,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract: `samples_len` samples, each its bytes for the call. Tested by
            // test/t/106_cport_simc.pl: simc::tests::each_served_entry_answers_as_the_simulations_host.
            let samples: Vec<&[u8]> = unsafe {
                let held: &[ffi::StdBytes] = match samples_len {
                    0 => &[],
                    len => std::slice::from_raw_parts(samples, len),
                };
                held.iter().map(|sample| c::slice(sample.ptr, sample.len)).collect()
            };
            match SimHost.train(&samples, capacity) {
                None => false,
                Some(dictionary) => {
                    let made = c::vec(&dictionary);
                    // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
                    // simc::tests::each_served_entry_answers_as_the_simulations_host.
                    unsafe { result.write(made) };
                    true
                }
            }
        })
    }

    /// cache_get is `Host::cache_get`.
    pub(super) unsafe extern "C" fn cache_get(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        placement: ffi::HostCachePlacement,
        key: *const ffi::HostCacheKey,
        has_result: *mut bool,
        result: *mut ffi::HostCacheFound,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a key the C made. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            let key = c::key(unsafe { &*key });
            let answer = SimHost.cache_get(&MayRaise::testing(), c::placement(placement), &key);
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            unsafe { maybe(answer.map(|found| found.map(|found| c::found(&found))), has_result, result, error) }
        })
    }

    /// cache_install is `Host::cache_install`, its fill the C's, which runs inside the C's catch so that a C panic in
    /// it unwinds the simulation's frames as a Rust panic carrying the C's payload.
    pub(super) unsafe extern "C" fn cache_install(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        placement: ffi::HostCachePlacement,
        key: *const ffi::HostCacheKey,
        len: usize,
        fill: ::core::option::Option<unsafe extern "C" fn(*mut c_void, *mut u8, usize)>,
        fill_captures: *mut c_void,
        has_result: *mut bool,
        result: *mut ffi::HostCacheFound,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!(saved => {
            let Some(fill) = fill else { unreachable!("the C door passes a fill") };
            // SAFETY: the C door's contract, a key the C made. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            let key = c::key(unsafe { &*key });
            let mut through = |bytes: &mut [u8]| {
                let mut filling = c::Filling {
                    fill,
                    captures: fill_captures,
                    bytes: bytes.as_mut_ptr(),
                    len: bytes.len(),
                    state: saved,
                };
                // SAFETY: the C door's contract, a fill of its captures, called once with the bytes the simulation
                // lends it. Tested by test/t/106_cport_simc.pl:
                // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame; test/t/106_cport_simc.pl:
                // simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload.
                unsafe { c::filled(&mut filling) }
            };
            let answer = SimHost.cache_install(&MayRaise::testing(), c::placement(placement), &key, len, &mut through);
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            unsafe { maybe(answer.map(|found| found.map(|found| c::found(&found))), has_result, result, error) }
        })
    }

    /// cache_contains is `Host::cache_contains`.
    pub(super) unsafe extern "C" fn cache_contains(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        placement: ffi::HostCachePlacement,
        key: *const ffi::HostCacheKey,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a key the C made. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            let key = c::key(unsafe { &*key });
            let answer = SimHost.cache_contains(&MayRaise::testing(), c::placement(placement), &key);
            // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
            unsafe { answered(answer, result, error) }
        })
    }

    /// cache_unpin is `Host::cache_unpin`.
    pub(super) unsafe extern "C" fn cache_unpin(_: *mut c_void, placement: ffi::HostCachePlacement, frame: u32) {
        entered!(SimHost.cache_unpin(c::placement(placement), frame))
    }

    /// cache_make_local is `Host::cache_make_local`.
    pub(super) unsafe extern "C" fn cache_make_local(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        bytes: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
        entered!(unsafe { failed(SimHost.cache_make_local(&MayRaise::testing(), bytes), error) })
    }

    /// owner_enlarge is `Host::owner_enlarge`.
    pub(super) unsafe extern "C" fn owner_enlarge(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame.
        entered!(unsafe { answered(SimHost.owner_enlarge(&MayRaise::testing()), result, error) })
    }

    /// owner_remember is `Host::owner_remember`.
    pub(super) unsafe extern "C" fn owner_remember(_: *mut c_void, owner: u64, pin: u64) {
        entered!(SimHost.owner_remember(owner, pin))
    }

    /// owner_forget is `Host::owner_forget`.
    pub(super) unsafe extern "C" fn owner_forget(_: *mut c_void, owner: u64, pin: u64) {
        entered!(SimHost.owner_forget(owner, pin))
    }
}

/// The entries simc doesn't serve yet, each failing with a named internal error rather than calling nothing.
//TODO: each entry over `SimHost`'s method as the crates' tests over sim reach it; the table is complete before the
// port is pushed for the owner's pull request.
mod unported {
    use super::*;

    /// unported fails a call of an entry simc doesn't serve yet with a named internal error.
    fn unported(entry: &str) -> ! {
        panic!("internal error: simc doesn't serve the simulation's host's {entry} yet")
    }

    /// wal_insert is `Host::wal_insert`.
    pub(super) unsafe extern "C" fn wal_insert(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: ffi::HostWalKind,
        body: *const u8,
        body_len: usize,
        result: *mut ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a body of its length that lives for the call and places for the answer.
            // Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host.
            unsafe {
                let body = c::slice(body, body_len);
                answered(SimHost.wal_insert(&MayRaise::testing(), c::wal_kind(kind), body), result, error)
            }
        })
    }

    /// wal_new_cid is `Host::wal_new_cid`.
    pub(super) unsafe extern "C" fn wal_new_cid(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        storage: *const ffi::HostStorage,
        tid: ffi::HostRow,
        cid: ffi::HostNewCid,
        result: *mut ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            let command = |has: bool, command: ffi::HostCommand| has.then_some(host::Command(command.f0));
            let cid = host::NewCid {
                cmin: command(cid.has_cmin, cid.cmin),
                cmax: command(cid.has_cmax, cid.cmax),
                combo: command(cid.has_combo, cid.combo),
            };
            // SAFETY: the C door's contract, a storage that lives for the call and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::the_new_cid_door_logs_its_record_as_the_simulations_host.
            unsafe {
                let ffi::HostStorage { tablespace, database, relation } = *storage;
                let storage = host::Storage { tablespace, database, relation };
                answered(SimHost.wal_new_cid(&MayRaise::testing(), &storage, host::Row(tid.f0), cid), result, error)
            }
        })
    }

    /// wal_flush is `Host::wal_flush`.
    pub(super) unsafe extern "C" fn wal_flush(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        lsn: ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
        // simc::tests::the_wal_flush_door_answers_as_the_simulations_host.
        entered!(unsafe { failed(SimHost.wal_flush(&MayRaise::testing(), lsn), error) })
    }

    /// fold_point is `Host::fold_point`.
    pub(super) unsafe extern "C" fn fold_point(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            let answer = SimHost.fold_point(&MayRaise::testing());
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/113_cport_txn.pl:
            // runs::tests::a_checkpoint_saves_its_runs_and_removes_the_unnamed.
            unsafe { answered(answer, result, error) }
        })
    }

    /// checkpoint_now is `Host::checkpoint_now`.
    pub(super) unsafe extern "C" fn checkpoint_now(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host.
            unsafe { failed(SimHost.checkpoint_now(&MayRaise::testing()), error) }
        })
    }

    /// quiet_database is `Host::quiet_database`.
    pub(super) unsafe extern "C" fn quiet_database(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        database: u32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host.
            unsafe { failed(SimHost.quiet_database(&MayRaise::testing(), database), error) }
        })
    }

    /// unquiet_database is `Host::unquiet_database`.
    pub(super) unsafe extern "C" fn unquiet_database(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        database: u32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host.
            unsafe { failed(SimHost.unquiet_database(&MayRaise::testing(), database), error) }
        })
    }

    /// last_redo is `Host::last_redo`.
    pub(super) unsafe extern "C" fn last_redo(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host.
            unsafe { answered(SimHost.last_redo(&MayRaise::testing()), result, error) }
        })
    }

    /// wal_stand is `Host::wal_stand`.
    pub(super) unsafe extern "C" fn wal_stand(_: *mut c_void) -> ffi::HostWalStand {
        entered!({
            let stood = SimHost.wal_stand();
            ffi::HostWalStand {
                recovering: stood.recovering,
                insertion_timeline: stood.insertion_timeline,
                flushed: stood.flushed,
                replayed: stood.replayed,
                replayed_timeline: stood.replayed_timeline,
                target_timeline: stood.target_timeline,
            }
        })
    }

    /// warn_kept_wal is `Host::warn_kept_wal`.
    pub(super) unsafe extern "C" fn warn_kept_wal(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        from: ffi::HostLsn,
        to: ffi::HostLsn,
        timeline: u32,
        upto: ffi::HostLsn,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them.
            unsafe { failed(SimHost.warn_kept_wal(&MayRaise::testing(), from, to, timeline, upto), error) }
        })
    }

    /// next_fork_record is `Host::next_fork_record`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn next_fork_record(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: ffi::HostLsn,
        _: u32,
        _: bool,
        _: *mut bool,
        _: *mut ffi::HostForkRecord,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("next_fork_record"))
    }

    /// warn is `Host::warn`.
    pub(super) unsafe extern "C" fn warn(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        warned: *mut ffi::HostError,
        context: *const c_char,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, an error the call takes, a NUL-terminated context that lives for the call,
            // and a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host.
            unsafe {
                let warned = c::error_from(warned);
                failed(SimHost.warn(&MayRaise::testing(), warned, CStr::from_ptr(context)), error)
            }
        })
    }

    /// log is `Host::log`.
    pub(super) unsafe extern "C" fn log(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        message: *const c_char,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a NUL-terminated message that lives for the call and a place for the
            // error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host.
            unsafe { failed(SimHost.log(&MayRaise::testing(), CStr::from_ptr(message)), error) }
        })
    }

    /// refuse_recovery is `Host::refuse_recovery`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn refuse_recovery(_: *mut c_void, _: *const c_char, _: usize) {
        entered!(unported("refuse_recovery"))
    }

    /// delay_checkpoints is `Host::delay_checkpoints`.
    pub(super) unsafe extern "C" fn delay_checkpoints(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        pauses: *const *const c_char,
        pauses_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, the pauses' NUL-terminated names of their length, each living as long as
            // the program, and a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_checkpoint_delay_doors_answer_as_the_simulations_host.
            unsafe {
                let pauses: Vec<&'static CStr> =
                    c::slice(pauses, pauses_len).iter().map(|&pause| CStr::from_ptr(pause)).collect();
                failed(SimHost.delay_checkpoints(&MayRaise::testing(), &pauses), error)
            }
        })
    }

    /// load_pauses loads nothing: the simulation's windows read their pauses at their open.
    pub(super) unsafe extern "C" fn load_pauses(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: *const *const c_char,
        _: usize,
        _: *mut ffi::HostError,
    ) -> bool {
        true
    }

    /// resume_checkpoints is `Host::resume_checkpoints`.
    pub(super) unsafe extern "C" fn resume_checkpoints(_: *mut c_void, finished: bool) {
        entered!(SimHost.resume_checkpoints(finished))
    }

    /// fail_checkpoints does nothing, leaving the error to its caller, as the C port's test host does.
    pub(super) unsafe extern "C" fn fail_checkpoints(_: *mut c_void, _: *const ffi::HostError) {}

    /// xid_status is `Host::xid_status`.
    pub(super) unsafe extern "C" fn xid_status(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        xid: ffi::HostXid,
        result: *mut ffi::HostStatus,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_transactions_status_answers_as_the_simulations_host.
            unsafe { answered(SimHost.xid_status(&MayRaise::testing(), Xid(xid.f0)).map(c::status), result, error) }
        })
    }

    /// xid_in_progress is `Host::xid_in_progress`.
    pub(super) unsafe extern "C" fn xid_in_progress(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        xid: ffi::HostXid,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_transactions_status_answers_as_the_simulations_host.
            unsafe { answered(SimHost.xid_in_progress(&MayRaise::testing(), Xid(xid.f0)), result, error) }
        })
    }

    /// xids_answered is `Host::xids_answered`.
    pub(super) unsafe extern "C" fn xids_answered(_: *mut c_void) -> bool {
        entered!(SimHost.xids_answered())
    }

    /// removable_in is `Host::removable_in`.
    pub(super) unsafe extern "C" fn removable_in(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        snapshot: ffi::StdAny,
        xid: ffi::HostXid,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            assert!(std::ptr::eq(snapshot.type_, &c::TAKEN.0), "a snapshot the C hands simc is one simc lent");
            // SAFETY: the C door's contract, a snapshot simc lent, whose own snapshot at `of` is the `Taken` it was lent
            // from and outlives the call, and places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable.
            unsafe {
                let taken = &*snapshot.value.cast::<sim::Taken>();
                answered(SimHost.removable_in(&MayRaise::testing(), taken, Xid(xid.f0)), result, error)
            }
        })
    }

    /// current_writer is `Host::current_writer`.
    pub(super) unsafe extern "C" fn current_writer(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result_0: *mut ffi::HostXid,
        result_1: *mut ffi::HostXid,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            let answer = SimHost.current_writer(&MayRaise::testing()).map(|(sub, top)| {
                // SAFETY: the C door's contract, a place for the top-level xid. Tested by test/t/106_cport_simc.pl:
                // simc::tests::an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one.
                unsafe { result_1.write(ffi::HostXid { f0: top.0 }) };
                ffi::HostXid { f0: sub.0 }
            });
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one.
            unsafe { answered(answer, result_0, error) }
        })
    }

    /// current_command is `Host::current_command`.
    pub(super) unsafe extern "C" fn current_command(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        used: bool,
        result: *mut ffi::HostCommand,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            let answer =
                SimHost.current_command(&MayRaise::testing(), used).map(|command| ffi::HostCommand { f0: command.0 });
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/113_cport_txn.pl:
            // visible::tests::a_transactions_own_row_is_seen_from_the_command_after_its_write.
            unsafe { answered(answer, result, error) }
        })
    }

    /// xid_is_current is `Host::xid_is_current`.
    pub(super) unsafe extern "C" fn xid_is_current(_: *mut c_void, xid: ffi::HostXid) -> bool {
        entered!(SimHost.xid_is_current(Xid(xid.f0)))
    }

    /// combo_command is `Host::combo_command`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn combo_command(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: ffi::HostXid,
        _: ffi::HostCommand,
        _: ffi::HostCommand,
        _: *mut ffi::HostCommand,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("combo_command"))
    }

    /// key_lock is `Host::key_lock`.
    pub(super) unsafe extern "C" fn key_lock(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        stripes: *const u32,
        stripes_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, the stripes of their length for the call and a place for the error.
            // Tested by test/t/106_cport_simc.pl: simc::tests::the_key_doors_answer_as_the_simulations_host.
            unsafe {
                let stripes = c::slice(stripes, stripes_len);
                answered(SimHost.key_lock(&MayRaise::testing(), stripes), &mut (), error)
            }
        })
    }

    /// key_unlock is `Host::key_unlock`.
    pub(super) unsafe extern "C" fn key_unlock(_: *mut c_void, stripes: *const u32, stripes_len: usize, pushed: bool) {
        entered!({
            // SAFETY: the C door's contract, the stripes of their length for the call. Tested by
            // test/t/106_cport_simc.pl: simc::tests::the_key_doors_answer_as_the_simulations_host.
            SimHost.key_unlock(unsafe { c::slice(stripes, stripes_len) }, pushed)
        })
    }

    /// key_count is `Host::key_count`.
    pub(super) unsafe extern "C" fn key_count(_: *mut c_void, stripe: u32) -> u64 {
        entered!(SimHost.key_count(stripe))
    }

    /// key_hold is `Host::key_hold`.
    pub(super) unsafe extern "C" fn key_hold(_: *mut c_void, stripe: u32) -> bool {
        entered!(SimHost.key_hold(stripe))
    }

    /// key_release is `Host::key_release`.
    pub(super) unsafe extern "C" fn key_release(_: *mut c_void, stripe: u32) {
        entered!(SimHost.key_release(stripe))
    }

    /// key_held_wait is `Host::key_held_wait`.
    pub(super) unsafe extern "C" fn key_held_wait(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        stripe: u32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_key_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.key_held_wait(&MayRaise::testing(), stripe), &mut (), error) }
        })
    }

    /// xact_wait is `Host::xact_wait`, which simc doesn't serve yet: a call fails with its named internal error.
    pub(super) unsafe extern "C" fn xact_wait(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: ffi::HostXid,
        _: bool,
        _: ffi::HostOperation,
        _: bool,
        _: *mut bool,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("xact_wait"))
    }

    /// speculative_wait is `Host::speculative_wait`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn speculative_wait(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: ffi::HostXid,
        _: u32,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("speculative_wait"))
    }

    /// lock_tuple is `Host::lock_tuple`, which simc doesn't serve yet: a call fails with its named internal error.
    pub(super) unsafe extern "C" fn lock_tuple(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: ffi::HostTupleLock,
        _: bool,
        _: *mut bool,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("lock_tuple"))
    }

    /// unlock_tuple is `Host::unlock_tuple`, which simc doesn't serve yet: a call fails with its named internal error.
    pub(super) unsafe extern "C" fn unlock_tuple(_: *mut c_void, _: ffi::HostTupleLock) {
        entered!(unported("unlock_tuple"))
    }

    /// pause is `Host::pause`.
    pub(super) unsafe extern "C" fn pause(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        point: *const c_char,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a NUL-terminated point name that lives for the call and a place for the
            // error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host.
            unsafe { failed(SimHost.pause(&MayRaise::testing(), CStr::from_ptr(point)), error) }
        })
    }

    /// injected is `Host::injected`.
    pub(super) unsafe extern "C" fn injected(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        point: *const c_char,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a NUL-terminated point name that lives for the call and places for the
            // answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_injected_door_answers_as_the_simulations_host.
            unsafe { answered(SimHost.injected(&MayRaise::testing(), CStr::from_ptr(point)), result, error) }
        })
    }

    /// shared_publish is `Host::shared_publish`.
    pub(super) unsafe extern "C" fn shared_publish(_: *mut c_void, place: u64) -> u64 {
        entered!(SimHost.shared_publish(place))
    }

    /// shared_claim is `Host::shared_claim`.
    pub(super) unsafe extern "C" fn shared_claim(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        location: u64,
        has_result: *mut bool,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.shared_claim(&kind, location));
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// shared_free is `Host::shared_free`.
    pub(super) unsafe extern "C" fn shared_free(_: *mut c_void, place: u64) {
        entered!(SimHost.shared_free(place))
    }

    /// shared_give_up is `Host::shared_give_up`.
    pub(super) unsafe extern "C" fn shared_give_up(_: *mut c_void, place: u64) {
        entered!(SimHost.shared_give_up(place))
    }

    /// shared_close is `Host::shared_close`.
    pub(super) unsafe extern "C" fn shared_close(_: *mut c_void, place: u64) {
        entered!(SimHost.shared_close(place))
    }

    /// shared_reclaim is `Host::shared_reclaim`.
    pub(super) unsafe extern "C" fn shared_reclaim(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        location: u64,
        result: *mut ffi::HostSharedLooked,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.shared_reclaim(&kind, location));
                answered(answer.map(c::looked), result, error)
            }
        })
    }

    /// shared_reclaimed is `Host::shared_reclaimed`.
    pub(super) unsafe extern "C" fn shared_reclaimed(_: *mut c_void, place: u64) -> bool {
        entered!(SimHost.shared_reclaimed(place))
    }

    /// shared_unreclaim is `Host::shared_unreclaim`.
    pub(super) unsafe extern "C" fn shared_unreclaim(_: *mut c_void, place: u64) {
        entered!(SimHost.shared_unreclaim(place))
    }

    /// shared_sweep is `Host::shared_sweep`.
    pub(super) unsafe extern "C" fn shared_sweep(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        atomic: usize,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.shared_sweep(&MayRaise::testing(), &kind, atomic));
                answered(answer.map(|pairs| c::pairs(&pairs)), result, error)
            }
        })
    }

    /// shared_index_find is `sim::index_find`.
    pub(super) unsafe extern "C" fn shared_index_find(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        key: *const u8,
        has: *mut bool,
        location: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a pool's kind of the C's with an index, a key of its index's length, and
            // places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pools_index_answers_as_the_simulations_host.
            unsafe {
                let key = std::slice::from_raw_parts(key, (*kind).index);
                match c::kind(&*kind).and_then(|kind| sim::index_find(&kind, key)) {
                    Ok(found) => {
                        has.write(found.is_some());
                        location.write(found.unwrap_or(0));
                        true
                    }
                    Err(failure) => failed(Err(failure), error),
                }
            }
        })
    }

    /// shared_index_put is `sim::index_put`.
    pub(super) unsafe extern "C" fn shared_index_put(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        key: *const u8,
        location: u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a pool's kind of the C's with an index, a key of its index's length, and
            // a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pools_index_answers_as_the_simulations_host.
            unsafe {
                let key = std::slice::from_raw_parts(key, (*kind).index);
                failed(c::kind(&*kind).and_then(|kind| sim::index_put(&kind, key, location)), error)
            }
        })
    }

    /// shared_index_remove is `sim::index_remove`.
    pub(super) unsafe extern "C" fn shared_index_remove(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        key: *const u8,
        location: u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a pool's kind of the C's with an index, a key of its index's length, and
            // a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pools_index_answers_as_the_simulations_host.
            unsafe {
                let key = std::slice::from_raw_parts(key, (*kind).index);
                failed(c::kind(&*kind).and_then(|kind| sim::index_remove(&kind, key, location)), error)
            }
        })
    }

    /// shared_single is `Host::shared_single`.
    pub(super) unsafe extern "C" fn shared_single(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { answered(c::kind(&*kind).and_then(|kind| SimHost.shared_single(&kind)), result, error) }
        })
    }

    /// shared_ready is `Host::shared_ready`.
    pub(super) unsafe extern "C" fn shared_ready(_: *mut c_void) -> bool {
        entered!(SimHost.shared_ready())
    }

    /// shared_lock is `Host::shared_lock`.
    pub(super) unsafe extern "C" fn shared_lock(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        place: u64,
        lock: usize,
        exclusive: bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { failed(SimHost.shared_lock(&MayRaise::testing(), place, lock, exclusive), error) }
        })
    }

    /// shared_unlock is `Host::shared_unlock`.
    pub(super) unsafe extern "C" fn shared_unlock(_: *mut c_void, place: u64, lock: usize) {
        entered!(SimHost.shared_unlock(place, lock))
    }

    /// atomic is `Host::atomic`.
    pub(super) unsafe extern "C" fn atomic(_: *mut c_void, place: u64, atomic: usize, op: ffi::HostSharedOp) -> u64 {
        entered!(SimHost.atomic(place, atomic, c::op(op)))
    }

    /// shared_atomics is `Host::shared_atomics`.
    pub(super) unsafe extern "C" fn shared_atomics(_: *mut c_void, place: u64) -> usize {
        entered!(SimHost.shared_atomics(place))
    }

    /// blob_read is `Host::blob_read`.
    pub(super) unsafe extern "C" fn blob_read(_: *mut c_void, place: u64, blob: usize) -> ffi::StdVec {
        entered!(c::vec(&SimHost.blob_read(place, blob)))
    }

    /// blob_write is `Host::blob_write`.
    pub(super) unsafe extern "C" fn blob_write(
        _: *mut c_void,
        place: u64,
        blob: usize,
        bytes: *const u8,
        bytes_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, bytes that live for the call and a place for the error. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { failed(SimHost.blob_write(place, blob, c::slice(bytes, bytes_len)), error) }
        })
    }

    /// entry_read is `Host::entry_read`.
    pub(super) unsafe extern "C" fn entry_read(
        _: *mut c_void,
        place: u64,
        array: usize,
        entry: usize,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.entry_read(place, array, entry).map(|bytes| c::vec(&bytes)), result, error) }
        })
    }

    /// entry_write is `Host::entry_write`.
    pub(super) unsafe extern "C" fn entry_write(
        _: *mut c_void,
        place: u64,
        array: usize,
        entry: usize,
        bytes: *const u8,
        bytes_len: usize,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, bytes that live for the call and a place for the error. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { failed(SimHost.entry_write(place, array, entry, c::slice(bytes, bytes_len)), error) }
        })
    }

    /// shared_location is `Host::shared_location`.
    pub(super) unsafe extern "C" fn shared_location(_: *mut c_void, place: u64) -> u64 {
        entered!(SimHost.shared_location(place))
    }

    /// shared_live is `Host::shared_live`.
    pub(super) unsafe extern "C" fn shared_live(_: *mut c_void, location: u64) -> bool {
        entered!(SimHost.shared_live(location))
    }

    /// shared_gone is `Host::shared_gone`.
    pub(super) unsafe extern "C" fn shared_gone(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        location: u64,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_pooled_records_doors_answer_as_the_simulations_host.
            unsafe { answered(c::kind(&*kind).and_then(|kind| SimHost.shared_gone(&kind, location)), result, error) }
        })
    }

    /// words_processes is `Host::words_processes`.
    pub(super) unsafe extern "C" fn words_processes(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        result: *mut ffi::StdVec,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_words_answer_as_the_simulations_host.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.words_processes(&kind));
                answered(answer.map(|processes| c::numbers(&processes)), result, error)
            }
        })
    }

    /// words_mine is `Host::words_mine`.
    pub(super) unsafe extern "C" fn words_mine(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_words_answer_as_the_simulations_host.
            unsafe { answered(c::kind(&*kind).and_then(|kind| SimHost.words_mine(&kind)), result, error) }
        })
    }

    /// word_store is `Host::word_store`.
    pub(super) unsafe extern "C" fn word_store(_: *mut c_void, place: u64, word: usize, value: u64) {
        entered!(SimHost.word_store(place, word, value))
    }

    /// word_store_own is `Host::word_store_own`.
    pub(super) unsafe extern "C" fn word_store_own(
        _: *mut c_void,
        index: usize,
        kind: *const ffi::HostSharedKind,
        word: usize,
        value: u64,
    ) {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's, which is one of the server's table of kinds. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_words_answer_as_the_simulations_host.
            let kind = unsafe { c::kind(&*kind) }.expect("a kind in the server's table converts");
            SimHost.word_store_own(index, &kind, word, value)
        })
    }

    /// word_load is `Host::word_load`.
    pub(super) unsafe extern "C" fn word_load(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        proc: u32,
        word: usize,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_words_answer_as_the_simulations_host.
            unsafe { answered(c::kind(&*kind).and_then(|kind| SimHost.word_load(&kind, proc, word)), result, error) }
        })
    }

    /// words_place is `Host::words_place`.
    pub(super) unsafe extern "C" fn words_place(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        proc: u32,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl: simc::tests::a_processs_words_answer_as_the_simulations_host.
            unsafe { answered(c::kind(&*kind).and_then(|kind| SimHost.words_place(&kind, proc)), result, error) }
        })
    }

    /// epochs_lock is `Host::epochs_lock`.
    pub(super) unsafe extern "C" fn epochs_lock(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_epoch_doors_answer_as_the_simulations_host.
            unsafe { failed(SimHost.epochs_lock(&MayRaise::testing()), error) }
        })
    }

    /// epochs_unlock is `Host::epochs_unlock`.
    pub(super) unsafe extern "C" fn epochs_unlock(_: *mut c_void) {
        entered!(SimHost.epochs_unlock())
    }

    /// epochs_advance is `Host::epochs_advance`.
    pub(super) unsafe extern "C" fn epochs_advance(_: *mut c_void) -> u64 {
        entered!(SimHost.epochs_advance())
    }

    /// epochs_oldest is `Host::epochs_oldest`.
    pub(super) unsafe extern "C" fn epochs_oldest(_: *mut c_void) -> u64 {
        entered!(SimHost.epochs_oldest())
    }

    /// epochs_read is `Host::epochs_read`.
    pub(super) unsafe extern "C" fn epochs_read(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_epoch_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.epochs_read(&MayRaise::testing()), result, error) }
        })
    }

    /// epochs_end is `Host::epochs_end`.
    pub(super) unsafe extern "C" fn epochs_end(_: *mut c_void, epoch: u64) {
        entered!(SimHost.epochs_end(epoch))
    }

    /// delta_hold is `Host::delta_hold`.
    pub(super) unsafe extern "C" fn delta_hold(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_delta_hold_and_budget_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.delta_hold(&MayRaise::testing()), result, error) }
        })
    }

    /// delta_release is `Host::delta_release`.
    pub(super) unsafe extern "C" fn delta_release(_: *mut c_void, epoch: u64) {
        entered!(SimHost.delta_release(epoch))
    }

    /// delta_open is `Host::delta_open`.
    pub(super) unsafe extern "C" fn delta_open(
        _: *mut c_void,
        name: ffi::HostDeltaPagePage,
        epoch: u64,
        result: *mut ffi::HostDeltaDirectoryOpened,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe { answered(SimHost.delta_open(c::page(name), epoch).map(c::opened_page), result, error) }
        })
    }

    /// delta_bytes is `Host::delta_bytes`.
    pub(super) unsafe extern "C" fn delta_bytes(
        _: *mut c_void,
        opened: *const ffi::HostDeltaDirectoryOpened,
        offset: u64,
        len: u64,
        result: *mut ffi::HostCacheBytes,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, an opened page for the call and places for the answer. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe {
                let answer = SimHost.delta_bytes(&c::opened(&*opened), offset, len);
                answered(answer.map(|bytes| c::cache_bytes(&bytes)), result, error)
            }
        })
    }

    /// delta_room is `Host::delta_room`.
    pub(super) unsafe extern "C" fn delta_room(_: *mut c_void, opened: *const ffi::HostDeltaDirectoryOpened) -> u64 {
        entered!({
            // SAFETY: the C door's contract, an opened page for the call. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe { SimHost.delta_room(&c::opened(&*opened)) }
        })
    }

    /// delta_filled is `Host::delta_filled`.
    pub(super) unsafe extern "C" fn delta_filled(_: *mut c_void, opened: *const ffi::HostDeltaDirectoryOpened) -> u64 {
        entered!({
            // SAFETY: the C door's contract, an opened page for the call. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe { SimHost.delta_filled(&c::opened(&*opened)) }
        })
    }

    /// delta_word is `Host::delta_word`.
    pub(super) unsafe extern "C" fn delta_word(_: *mut c_void, body: u64, index: u64, op: ffi::HostDeltaWordOp) -> u64 {
        entered!(SimHost.delta_word(body, index, c::word_op(op)))
    }

    /// delta_alloc is `Host::delta_alloc`.
    pub(super) unsafe extern "C" fn delta_alloc(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        sizes: *const ffi::HostDeltaPageSize,
        sizes_len: usize,
        names: *mut ffi::StdVec,
        has_result: *mut bool,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract: `sizes_len` sizes for the call, a vector of names with room past its
            // length for one per size, and places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host.
            unsafe {
                let held: &[ffi::HostDeltaPageSize] = match sizes_len {
                    0 => &[],
                    len => std::slice::from_raw_parts(sizes, len),
                };
                let sizes: Vec<_> = held.iter().map(|size| c::size(*size)).collect();
                let mut taken = Vec::new();
                let answer = SimHost.delta_alloc(&MayRaise::testing(), &sizes, &mut taken);
                c::pages_into(names, &taken);
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// delta_alloc_replacing is `Host::delta_alloc_replacing`.
    pub(super) unsafe extern "C" fn delta_alloc_replacing(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        size: ffi::HostDeltaPageSize,
        replaced: ffi::HostDeltaPagePage,
        names: *mut ffi::StdVec,
        has_result: *mut bool,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a vector of names with room past its length for one, and places for the
            // answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken.
            unsafe {
                let mut taken = Vec::new();
                let answer =
                    SimHost.delta_alloc_replacing(&MayRaise::testing(), c::size(size), c::page(replaced), &mut taken);
                c::pages_into(names, &taken);
                maybe(answer, has_result, result, error)
            }
        })
    }

    /// delta_give_back is `Host::delta_give_back`.
    pub(super) unsafe extern "C" fn delta_give_back(_: *mut c_void, name: ffi::HostDeltaPagePage) {
        entered!(SimHost.delta_give_back(c::page(name)))
    }

    /// delta_owned_append is `Host::delta_owned_append`.
    pub(super) unsafe extern "C" fn delta_owned_append(
        _: *mut c_void,
        name: ffi::HostDeltaPagePage,
        record: *const u8,
        record_len: usize,
        result: *mut u32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a record of its length for the call and places for the answer. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host.
            unsafe {
                let record = c::slice(record, record_len);
                answered(SimHost.delta_owned_append(c::page(name), record), result, error)
            }
        })
    }

    /// delta_owned_store is `Host::delta_owned_store`.
    pub(super) unsafe extern "C" fn delta_owned_store(
        _: *mut c_void,
        name: ffi::HostDeltaPagePage,
        index: u64,
        value: u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host.
            unsafe { failed(SimHost.delta_owned_store(c::page(name), index, value), error) }
        })
    }

    /// delta_link is `Host::delta_link`.
    pub(super) unsafe extern "C" fn delta_link(
        _: *mut c_void,
        name: ffi::HostDeltaPagePage,
        owner: ffi::HostDeltaDirectoryOwner,
    ) {
        entered!(SimHost.delta_link(c::page(name), c::owner(owner)))
    }

    /// delta_append is `Host::delta_append`.
    pub(super) unsafe extern "C" fn delta_append(
        _: *mut c_void,
        opened: *const ffi::HostDeltaDirectoryOpened,
        owner: ffi::HostDeltaDirectoryOwner,
        record: *const u8,
        record_len: usize,
        result: *mut u32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, an opened page and a record of its length for the call, and places for the
            // answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe {
                let record = c::slice(record, record_len);
                answered(SimHost.delta_append(&c::opened(&*opened), c::owner(owner), record), result, error)
            }
        })
    }

    /// delta_seal is `Host::delta_seal`.
    pub(super) unsafe extern "C" fn delta_seal(
        _: *mut c_void,
        opened: *const ffi::HostDeltaDirectoryOpened,
        owner: ffi::HostDeltaDirectoryOwner,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, an opened page for the call and a place for the error. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe { failed(SimHost.delta_seal(&c::opened(&*opened), c::owner(owner)), error) }
        })
    }

    /// delta_retire is `Host::delta_retire`.
    pub(super) unsafe extern "C" fn delta_retire(
        _: *mut c_void,
        records: *const ffi::HostDeltaLeaving,
        records_len: usize,
        names: *const ffi::HostDeltaPagePage,
        names_len: usize,
    ) {
        entered!({
            // SAFETY: the C door's contract, `records_len` records and `names_len` names for the call. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            let (records, names) = unsafe {
                let records: &[ffi::HostDeltaLeaving] = match records_len {
                    0 => &[],
                    len => std::slice::from_raw_parts(records, len),
                };
                let names: &[ffi::HostDeltaPagePage] = match names_len {
                    0 => &[],
                    len => std::slice::from_raw_parts(names, len),
                };
                (records, names)
            };
            let records: Vec<_> = records.iter().map(|record| Leaving::raw(record.id, record.location)).collect();
            let names: Vec<_> = names.iter().map(|name| c::page(*name)).collect();
            SimHost.delta_retire(&records, &names)
        })
    }

    /// delta_make is `Host::delta_make`.
    pub(super) unsafe extern "C" fn delta_make(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        kind: *const ffi::HostSharedKind,
        name: *const c_char,
        name_len: usize,
        result_ok: *mut bool,
        result_0: *mut u64,
        result_1: *mut u64,
        result_err: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and its name of its length for the call, and places for
            // the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe {
                let name =
                    std::str::from_utf8(c::slice::<u8>(name.cast(), name_len)).expect("the C's kind names are UTF-8");
                match c::kind(&*kind).and_then(|kind| SimHost.delta_make(&MayRaise::testing(), &kind, name)) {
                    Ok(Ok((place, location))) => {
                        result_ok.write(true);
                        result_0.write(place);
                        result_1.write(location);
                        true
                    }
                    Ok(Err(frees)) => {
                        result_ok.write(false);
                        result_err.write(frees);
                        true
                    }
                    Err(failure) => failed(Err(failure), error),
                }
            }
        })
    }

    /// delta_record is `Host::delta_record`.
    pub(super) unsafe extern "C" fn delta_record(
        _: *mut c_void,
        kind: *const ffi::HostSharedKind,
        location: u64,
        epoch: u64,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a kind of the C's and places for the answer. Tested by
            // test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe {
                let answer = c::kind(&*kind).and_then(|kind| SimHost.delta_record(&kind, location, epoch));
                answered(answer, result, error)
            }
        })
    }

    /// delta_publish is `Host::delta_publish`.
    pub(super) unsafe extern "C" fn delta_publish(_: *mut c_void, place: u64) {
        entered!(SimHost.delta_publish(place))
    }

    /// delta_unmake is `Host::delta_unmake`.
    pub(super) unsafe extern "C" fn delta_unmake(_: *mut c_void, place: u64) {
        entered!(SimHost.delta_unmake(place))
    }

    /// delta_pass is `Host::delta_pass`.
    pub(super) unsafe extern "C" fn delta_pass(_: *mut c_void) {
        entered!(SimHost.delta_pass())
    }

    /// delta_advanced is `Host::delta_advanced`.
    pub(super) unsafe extern "C" fn delta_advanced(_: *mut c_void, passed: *mut u64, oldest: *mut u64) {
        entered!({
            let (now, before) = SimHost.delta_advanced();
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host.
            unsafe {
                passed.write(now);
                oldest.write(before);
            }
        })
    }

    /// delta_used is `Host::delta_used`.
    pub(super) unsafe extern "C" fn delta_used(_: *mut c_void) -> u64 {
        entered!(SimHost.delta_used())
    }

    /// delta_budget is `Host::delta_budget`.
    pub(super) unsafe extern "C" fn delta_budget(_: *mut c_void) -> u64 {
        entered!(SimHost.delta_budget())
    }

    /// delta_seed is `Host::delta_seed`.
    pub(super) unsafe extern "C" fn delta_seed(_: *mut c_void, result: *mut u8) {
        entered!({
            let seed = SimHost.delta_seed();
            // SAFETY: the C door's contract, a place for the seed's 32 bytes. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host.
            unsafe { result.copy_from_nonoverlapping(seed.as_ptr(), seed.len()) }
        })
    }

    /// delta_wait is `Host::delta_wait`.
    pub(super) unsafe extern "C" fn delta_wait(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        frees: u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host.
            unsafe { failed(SimHost.delta_wait(&MayRaise::testing(), frees), error) }
        })
    }

    /// delta_packing is the simulation's packing of the delta's blocks, which moves nothing: the simulation's delta,
    /// host's Rust directory, has no packing (bugs row 143).
    pub(super) unsafe extern "C" fn delta_packing(
        _: *mut c_void,
        _: *const u64,
        _: usize,
        _: *mut ffi::StdVec,
    ) -> bool {
        entered!(true)
    }

    /// backup_begin is `Host::backup_begin`.
    pub(super) unsafe extern "C" fn backup_begin(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a place for the error. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host.
            unsafe { failed(SimHost.backup_begin(&MayRaise::testing()), error) }
        })
    }

    /// backup_end is `Host::backup_end`.
    pub(super) unsafe extern "C" fn backup_end(_: *mut c_void) {
        entered!(SimHost.backup_end())
    }

    /// collection_begin is `Host::collection_begin`.
    pub(super) unsafe extern "C" fn collection_begin(_: *mut c_void) -> bool {
        entered!(SimHost.collection_begin())
    }

    /// collection_end is `Host::collection_end`.
    pub(super) unsafe extern "C" fn collection_end(_: *mut c_void) {
        entered!(SimHost.collection_end())
    }

    /// collection_ask is `Host::collection_ask`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn collection_ask(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: u32,
        _: u32,
        _: *mut u64,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("collection_ask"))
    }

    /// collection_asked is `Host::collection_asked`, which simc doesn't serve yet: a call fails with its named internal
    /// error.
    pub(super) unsafe extern "C" fn collection_asked(_: *mut c_void, _: *mut u64, _: *mut u32, _: *mut u32) -> bool {
        entered!(unported("collection_asked"))
    }

    /// collection_answer is `Host::collection_answer`, which simc doesn't serve yet: a call fails with its named
    /// internal error.
    pub(super) unsafe extern "C" fn collection_answer(_: *mut c_void, _: u64, _: *const u64) {
        entered!(unported("collection_answer"))
    }

    /// collector_started is `Host::collector_started`, which simc doesn't serve yet: a call fails with its named
    /// internal error.
    pub(super) unsafe extern "C" fn collector_started(_: *mut c_void) {
        entered!(unported("collector_started"))
    }

    /// collector_running is `Host::collector_running`, which simc doesn't serve yet: a call fails with its named
    /// internal error.
    pub(super) unsafe extern "C" fn collector_running(_: *mut c_void) -> bool {
        entered!(unported("collector_running"))
    }

    /// database_locked is `Host::database_locked`.
    pub(super) unsafe extern "C" fn database_locked(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        database: u32,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.database_locked(&MayRaise::testing(), database), result, error) }
        })
    }

    /// database_unlocked is `Host::database_unlocked`.
    pub(super) unsafe extern "C" fn database_unlocked(_: *mut c_void, database: u32) {
        entered!(SimHost.database_unlocked(database))
    }

    /// database_lock_waited is `Host::database_lock_waited`.
    pub(super) unsafe extern "C" fn database_lock_waited(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        database: u32,
        result: *mut bool,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host.
            unsafe { answered(SimHost.database_lock_waited(&MayRaise::testing(), database), result, error) }
        })
    }

    /// owner_top_enlarge is `Host::owner_top_enlarge`.
    pub(super) unsafe extern "C" fn owner_top_enlarge(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        result: *mut u64,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::an_owned_record_is_released_by_its_owners_abort_unless_forgotten.
            unsafe { answered(SimHost.owner_top_enlarge(&MayRaise::testing()), result, error) }
        })
    }

    /// owned_remember is `Host::owned_remember`, the C kind's release called through its slot.
    pub(super) unsafe extern "C" fn owned_remember(
        _: *mut c_void,
        owner: u64,
        record: u64,
        release: ::core::option::Option<unsafe extern "C" fn(u32)>,
    ) {
        entered!(SimHost.owned_remember(owner, record, c::release_of(release)))
    }

    /// owned_forget is `Host::owned_forget`.
    pub(super) unsafe extern "C" fn owned_forget(_: *mut c_void, owner: u64, record: u64) {
        entered!(SimHost.owned_forget(owner, record))
    }

    /// btree_compare is `Host::btree_compare`.
    pub(super) unsafe extern "C" fn btree_compare(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        compared: *const ffi::HostCompared,
        a: *const u8,
        a_len: usize,
        b: *const u8,
        b_len: usize,
        result: *mut i32,
        error: *mut ffi::HostError,
    ) -> bool {
        entered!({
            // SAFETY: the C door's contract, a column's call and two values of their lengths that live for the call,
            // and places for the answer. Tested by test/t/106_cport_simc.pl:
            // simc::tests::the_btree_compare_door_answers_as_the_simulations_host.
            unsafe {
                let ffi::HostCompared { function, collation, len, by_value } = *compared;
                let compared = host::Compared { function, collation, len, by_value };
                let (a, b) = (c::slice(a, a_len), c::slice(b, b_len));
                answered(SimHost.btree_compare(&MayRaise::testing(), &compared, a, b), result, error)
            }
        })
    }

    /// sort_key is `Host::sort_key`, which simc doesn't serve yet: a call fails with its named internal error.
    pub(super) unsafe extern "C" fn sort_key(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: u32,
        _: *const u8,
        _: usize,
        _: *mut ffi::StdVec,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("sort_key"))
    }

    /// reformed is `Host::reformed`, which simc doesn't serve yet: a call fails with its named internal error.
    pub(super) unsafe extern "C" fn reformed(
        _: *mut c_void,
        _: *const ffi::HostMayRaise,
        _: *const ffi::HostReformRow,
        _: *mut ffi::HostReformedRow,
        _: *mut ffi::HostError,
    ) -> bool {
        entered!(unported("reformed"))
    }
}

/// HOST_OPS is the simulation's host as the C port's host table.
pub(crate) static HOST_OPS: ffi::HostHostOps = ffi::HostHostOps {
    read_host: ffi::HostReadHostOps {
        open_to_read: Some(read::open_to_read),
        open_if_there: Some(read::open_if_there),
        read_at: Some(read::read_at),
        size: Some(read::size),
        prefetch: Some(read::prefetch),
        close: Some(read::close),
        tablespace_root: Some(read::tablespace_root),
        list_dir: Some(read::list_dir),
        list_dir_if_there: Some(read::list_dir_if_there),
        prepare_decompressing: Some(read::prepare_decompressing),
        frame_size: Some(read::frame_size),
        decompress: Some(read::decompress),
    },
    now: Some(write::now),
    instant: Some(write::instant),
    process_id: Some(write::process_id),
    work_mem: Some(write::work_mem),
    session_database: Some(write::session_database),
    exiting: Some(write::exiting),
    unique: Some(write::unique),
    check_interrupts: Some(write::check_interrupts),
    hold_interrupts: Some(write::hold_interrupts),
    resume_interrupts: Some(write::resume_interrupts),
    open_: Some(write::open),
    create_if_absent: Some(write::create_if_absent),
    open_scratch: Some(write::open_scratch),
    write_at: Some(write::write_at),
    truncate: Some(write::truncate),
    sync: Some(write::sync),
    rename_: Some(write::rename),
    rename_durably: Some(write::rename_durably),
    remove_durably: Some(write::remove_durably),
    remove_durably_if_there: Some(write::remove_durably_if_there),
    remove_leftover: Some(write::remove_leftover),
    rename_back: Some(write::rename_back),
    remove_tree: Some(write::remove_tree),
    sync_path: Some(write::sync_path),
    make_dir: Some(write::make_dir),
    disk_space: Some(write::disk_space),
    tree_bytes: Some(write::tree_bytes),
    nap: Some(write::nap),
    actor: Some(write::actor),
    yield_point: Some(write::yield_point),
    wal_insert: Some(unported::wal_insert),
    wal_new_cid: Some(unported::wal_new_cid),
    wal_flush: Some(unported::wal_flush),
    fold_point: Some(unported::fold_point),
    checkpoint_now: Some(unported::checkpoint_now),
    quiet_database: Some(unported::quiet_database),
    unquiet_database: Some(unported::unquiet_database),
    last_redo: Some(unported::last_redo),
    wal_stand: Some(unported::wal_stand),
    warn_kept_wal: Some(unported::warn_kept_wal),
    next_fork_record: Some(unported::next_fork_record),
    warn: Some(unported::warn),
    log: Some(unported::log),
    refuse_recovery: Some(unported::refuse_recovery),
    delay_checkpoints: Some(unported::delay_checkpoints),
    load_pauses: Some(unported::load_pauses),
    resume_checkpoints: Some(unported::resume_checkpoints),
    fail_checkpoints: Some(unported::fail_checkpoints),
    xid_status: Some(unported::xid_status),
    xid_in_progress: Some(unported::xid_in_progress),
    xids_answered: Some(unported::xids_answered),
    removable_in: Some(unported::removable_in),
    current_writer: Some(unported::current_writer),
    current_command: Some(unported::current_command),
    xid_is_current: Some(unported::xid_is_current),
    combo_command: Some(unported::combo_command),
    key_lock: Some(unported::key_lock),
    key_unlock: Some(unported::key_unlock),
    key_count: Some(unported::key_count),
    key_hold: Some(unported::key_hold),
    key_release: Some(unported::key_release),
    key_held_wait: Some(unported::key_held_wait),
    xact_wait: Some(unported::xact_wait),
    speculative_wait: Some(unported::speculative_wait),
    lock_tuple: Some(unported::lock_tuple),
    unlock_tuple: Some(unported::unlock_tuple),
    pause: Some(unported::pause),
    injected: Some(unported::injected),
    at_exit: Some(write::at_exit),
    prepare_compressing: Some(write::prepare_compressing),
    compress: Some(write::compress),
    train: Some(write::train),
    shared_make: Some(write::shared_make),
    shared_publish: Some(unported::shared_publish),
    shared_claim: Some(unported::shared_claim),
    shared_free: Some(unported::shared_free),
    shared_give_up: Some(unported::shared_give_up),
    shared_open: Some(write::shared_open),
    shared_close: Some(unported::shared_close),
    shared_reclaim: Some(unported::shared_reclaim),
    shared_reclaimed: Some(unported::shared_reclaimed),
    shared_unreclaim: Some(unported::shared_unreclaim),
    shared_sweep: Some(unported::shared_sweep),
    shared_index_find: Some(unported::shared_index_find),
    shared_index_put: Some(unported::shared_index_put),
    shared_index_remove: Some(unported::shared_index_remove),
    shared_single: Some(unported::shared_single),
    shared_ready: Some(unported::shared_ready),
    shared_lock: Some(unported::shared_lock),
    shared_unlock: Some(unported::shared_unlock),
    flag_take: Some(write::flag_take),
    flag_try: Some(write::flag_try),
    flag_clear: Some(write::flag_clear),
    atomic: Some(unported::atomic),
    shared_atomics: Some(unported::shared_atomics),
    blob_read: Some(unported::blob_read),
    blob_write: Some(unported::blob_write),
    entry_read: Some(unported::entry_read),
    entry_write: Some(unported::entry_write),
    shared_location: Some(unported::shared_location),
    shared_live: Some(unported::shared_live),
    shared_gone: Some(unported::shared_gone),
    slots_name: Some(write::slots_name),
    slots_rename: Some(write::slots_rename),
    slots_empty: Some(write::slots_empty),
    slots_claim: Some(write::slots_claim),
    slots_read: Some(write::slots_read),
    words_processes: Some(unported::words_processes),
    words_mine: Some(unported::words_mine),
    word_store: Some(unported::word_store),
    word_store_own: Some(unported::word_store_own),
    word_load: Some(unported::word_load),
    words_place: Some(unported::words_place),
    epochs_lock: Some(unported::epochs_lock),
    epochs_unlock: Some(unported::epochs_unlock),
    epochs_advance: Some(unported::epochs_advance),
    epochs_oldest: Some(unported::epochs_oldest),
    epochs_read: Some(unported::epochs_read),
    epochs_end: Some(unported::epochs_end),
    proc_number: Some(write::proc_number),
    delta_hold: Some(unported::delta_hold),
    delta_release: Some(unported::delta_release),
    delta_open: Some(unported::delta_open),
    delta_bytes: Some(unported::delta_bytes),
    delta_room: Some(unported::delta_room),
    delta_filled: Some(unported::delta_filled),
    delta_word: Some(unported::delta_word),
    delta_alloc: Some(unported::delta_alloc),
    delta_alloc_replacing: Some(unported::delta_alloc_replacing),
    delta_give_back: Some(unported::delta_give_back),
    delta_owned_append: Some(unported::delta_owned_append),
    delta_owned_store: Some(unported::delta_owned_store),
    delta_link: Some(unported::delta_link),
    delta_append: Some(unported::delta_append),
    delta_seal: Some(unported::delta_seal),
    delta_retire: Some(unported::delta_retire),
    delta_packing: Some(unported::delta_packing),
    delta_make: Some(unported::delta_make),
    delta_record: Some(unported::delta_record),
    delta_publish: Some(unported::delta_publish),
    delta_unmake: Some(unported::delta_unmake),
    delta_pass: Some(unported::delta_pass),
    delta_advanced: Some(unported::delta_advanced),
    delta_used: Some(unported::delta_used),
    delta_budget: Some(unported::delta_budget),
    delta_seed: Some(unported::delta_seed),
    delta_wait: Some(unported::delta_wait),
    backup_begin: Some(unported::backup_begin),
    backup_end: Some(unported::backup_end),
    backups_running: Some(write::backups_running),
    collection_begin: Some(unported::collection_begin),
    collection_end: Some(unported::collection_end),
    collection_ask: Some(unported::collection_ask),
    collection_asked: Some(unported::collection_asked),
    collection_answer: Some(unported::collection_answer),
    collector_started: Some(unported::collector_started),
    collector_running: Some(unported::collector_running),
    database_locked: Some(unported::database_locked),
    database_unlocked: Some(unported::database_unlocked),
    database_lock_waited: Some(unported::database_lock_waited),
    cache_get: Some(write::cache_get),
    cache_install: Some(write::cache_install),
    cache_contains: Some(write::cache_contains),
    cache_unpin: Some(write::cache_unpin),
    cache_make_local: Some(write::cache_make_local),
    owner_enlarge: Some(write::owner_enlarge),
    owner_top_enlarge: Some(unported::owner_top_enlarge),
    owner_remember: Some(write::owner_remember),
    owner_forget: Some(write::owner_forget),
    owned_remember: Some(unported::owned_remember),
    owned_forget: Some(unported::owned_forget),
    btree_compare: Some(unported::btree_compare),
    sort_key: Some(unported::sort_key),
    reformed: Some(unported::reformed),
};
