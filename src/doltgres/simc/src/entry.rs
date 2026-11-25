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

//! The wrapper each of simc's entries runs its body under (the port text's section 13): the backend's state saved
//! before the body, which may hand the run to another actor inside the simulation, and restored after; and a Rust
//! panic caught, since none may unwind through `extern "C"`, and raised again in the C with the same payload once
//! every Rust value of the entry is dropped, so the C's unwinding leaves only frames that hold nothing to drop.

use std::any::Any;
use std::ffi::c_void;
use std::panic::{self, AssertUnwindSafe};

use crate::ffi;

unsafe extern "C" {
    /// std_resume_unwind is the C's resume_unwind, which unwinds with a payload and never returns.
    fn std_resume_unwind(payload: ffi::StdPayload, box_: *mut c_void) -> !;
}

/// Raised is a Rust panic an entry caught, as the C raises it: its payload's kind and its box of the C's.
#[repr(C)]
pub(crate) struct Raised {
    /// kind is the payload's kind in the C.
    kind: ffi::StdPayload,
    /// boxed is the payload's box of the C's, none for out of memory.
    boxed: *mut c_void,
}

/// entered runs an entry's body under `enter`, and raises a panic of its in the C as the entry's own last call; with
/// `saved =>`, the body reads the backend's state the entry found as `saved`.
///
/// The raise is a call of the `extern "C"` `raise` from the entry's own frame, a call with no unwind edge, so the C's
/// unwinding, which on Windows runs each frame's pending cleanups, finds none in the entry's frame.
macro_rules! entered {
    ($saved:ident => $body:expr) => {
        match $crate::entry::enter(|$saved| $body) {
            Ok(value) => value,
            Err(raised) => $crate::entry::raise(raised),
        }
    };
    ($body:expr) => {
        match $crate::entry::enter(|_| $body) {
            Ok(value) => value,
            Err(raised) => $crate::entry::raise(raised),
        }
    };
}
pub(crate) use entered;

/// enter runs an entry's body with the backend's state kept, which it lends the body, and returns a panic of the
/// body's as the C raises it.
///
/// The body owns every value the entry makes, so when it panics nothing of the entry's is left to drop.
pub(crate) fn enter<T>(body: impl FnOnce(&ffi::SimcBackend) -> T) -> Result<T, Raised> {
    // SAFETY: `simc_backend_save` reads the backend's globals without changing them. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    let saved = unsafe { ffi::simc_backend_save() };
    let result = panic::catch_unwind(AssertUnwindSafe(|| body(&saved)));
    // SAFETY: `saved` is this backend's state as the entry found it, which another actor's turn may have changed.
    // Tested by test/t/106_cport_simc.pl: simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts.
    unsafe { ffi::simc_backend_restore(&saved) };
    result.map_err(|payload| {
        let (kind, boxed) = panic::catch_unwind(AssertUnwindSafe(|| boxed_payload(payload)))
            .unwrap_or((ffi::StdPayload_STD_PAYLOAD_OUT_OF_MEMORY, std::ptr::null_mut()));
        Raised { kind, boxed }
    })
}

/// raise raises a caught Rust panic again in the C with its payload; it is `extern "C"` so that its callers' calls of
/// it have no unwind edge.
pub(crate) extern "C" fn raise(raised: Raised) -> ! {
    // SAFETY: `boxed` is a box of the C's holding the payload's kind, none for out of memory, which the unwind takes;
    // every Rust value of the entry is dropped, so the C's unwinding leaves frames that hold nothing. Tested by
    // test/t/106_cport_simc.pl: simc::tests::a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message.
    unsafe { std_resume_unwind(raised.kind, raised.boxed) }
}

/// OutOfMemory is the payload of a failed allocation of simc's for the C, which the C raises as its own.
pub(crate) struct OutOfMemory;

/// Caught is a C panic's payload a Rust unwind carries through the simulation's frames back to the entry, which
/// raises it in the C unchanged.
pub(crate) struct Caught(pub(crate) ffi::StdCaught);

// SAFETY: a caught payload's box is the C's, which only the unwind that carries it reads. Tested by
// test/t/106_cport_simc.pl: simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload.
unsafe impl Send for Caught {}

/// boxed_payload returns a payload's kind in the C and its box of the C's, which the payload's drop leaves alone.
fn boxed_payload(payload: Box<dyn Any + Send>) -> (ffi::StdPayload, *mut c_void) {
    if payload.is::<OutOfMemory>() {
        return (ffi::StdPayload_STD_PAYLOAD_OUT_OF_MEMORY, std::ptr::null_mut());
    }
    let payload = match payload.downcast::<Caught>() {
        Ok(caught) => return (caught.0.payload, caught.0.box_),
        Err(payload) => payload,
    };
    match payload.downcast::<String>() {
        Ok(text) => {
            let made = crate::c::string(&text);
            let block = boxed(made);
            if block.is_null() && made.cap != 0 {
                // SAFETY: `made`'s block is the one `c::string` just allocated, which nothing else holds. Tested by
                // test/t/106_cport_simc.pl: simc::tests::a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory.
                unsafe { ffi::std_global_dealloc(made.ptr, made.cap, 1) };
            }
            (ffi::StdPayload_STD_PAYLOAD_STRING, or_out_of_memory(block))
        }
        Err(payload) => {
            let text: &'static str = match payload.downcast::<&'static str>() {
                Ok(text) => *text,
                Err(_) => "Box<dyn Any>",
            };
            let block = boxed(ffi::StdPayloadStr { ptr: text.as_ptr().cast(), len: text.len() });
            (ffi::StdPayload_STD_PAYLOAD_STR, or_out_of_memory(block))
        }
    }
}

/// or_out_of_memory returns a box simc made, or panics with out of memory for one it couldn't.
fn or_out_of_memory(block: *mut c_void) -> *mut c_void {
    if block.is_null() {
        panic::panic_any(OutOfMemory);
    }
    block
}

/// boxed moves a value into a box the C's allocator makes, which the C frees, or returns null when it can't.
fn boxed<T>(value: T) -> *mut c_void {
    let block = crate::c::allocated(size_of::<T>(), align_of::<T>()).cast::<T>();
    if !block.is_null() {
        // SAFETY: `block` is a fresh block of `T`'s layout. Tested by test/t/106_cport_simc.pl:
        // simc::tests::a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message.
        unsafe { block.write(value) };
    }
    block.cast()
}
