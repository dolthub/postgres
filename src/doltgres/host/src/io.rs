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

//! The capability to do file I/O and to raise, which an entry gets once and which every storage lock borrows while it
//! is held, so that no file door runs under one (L1).

use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};

use crate::{Error, MayRaise, current};

/// Held is the part of an `Io` a storage lock's guard lends on: it takes further storage locks, and reaches no file
/// door. Only an `Io` has one, so a drop, which has none, can't take a lock through it.
pub struct Held(PhantomData<*mut ()>);

impl Held {
    /// testing returns a lock scope for this crate's tests of the locks, which reach no seam.
    #[cfg(test)]
    pub(crate) fn testing() -> Held {
        Held(PhantomData)
    }
}

/// InDrop is what a drop takes a lock under which no file I/O runs with: a take that finds the lock held by a frame the
/// drop unwinds through, or poisoned, gives nothing, and the drop leaves its work to the state's rebuild, so that a drop
/// never panics during an unwind and never rebuilds (H2).
#[derive(Default)]
pub struct InDrop(PhantomData<*mut ()>);

impl InDrop {
    /// new returns the lock scope of a drop.
    pub fn new() -> InDrop {
        InDrop(PhantomData)
    }
}

/// Io is an entry's capability to do file I/O and to raise, which every file door takes. It is neither cloned nor sent,
/// and is made only by `testing`. Only the whole `Io` hands out what reaches a catalog or user code, never a view of
/// it, so a door that reaches either takes the `Io` itself.
pub struct Io {
    /// held is the lock scope the guards of storage locks borrow.
    held: Held,
}

impl Io {
    /// check fails with Postgres' error once an interrupt is pending, such as a cancel of the statement, the check each
    /// item a long loop hands out makes (L2).
    pub fn check(&mut self) -> Result<(), Error> {
        current().check_interrupts(&MayRaise::of())
    }
}

impl Io {
    /// testing returns a test's capability, which only the simulation's host accepts.
    #[cfg(feature = "testing")]
    pub fn testing() -> Io {
        Io { held: Held(PhantomData) }
    }
}

impl Deref for Io {
    type Target = Held;

    fn deref(&self) -> &Held {
        &self.held
    }
}

impl DerefMut for Io {
    fn deref_mut(&mut self) -> &mut Held {
        &mut self.held
    }
}

/// Waits is an entry's capability to wait on another backend: on its transaction, on a heavyweight lock, on a
/// condition variable, or for the delta's budget, which every such door takes. It is neither cloned nor sent, and is
/// made only by `testing`, as `Io` is.
pub struct Waits(PhantomData<*mut ()>);

impl Waits {
    /// testing returns a test's capability, which only the simulation's host serves.
    #[cfg(feature = "testing")]
    pub fn testing() -> Waits {
        Waits(PhantomData)
    }
}

/// sealed keeps the views that do file I/O to the ones this module defines.
pub(crate) mod sealed {
    use super::Held;

    /// Sealed is implemented only by this crate's views of an `Io`.
    pub trait Sealed {
        /// parts returns the view's lock scope.
        fn parts(&mut self) -> &mut Held;
    }
}

/// ReadDoors is what the doors that only read take: any view that does file I/O.
pub trait ReadDoors: sealed::Sealed + DerefMut<Target = Held> {}

/// IoDoors is what the file doors that write take: an entry's whole `Io`, or the view a short lender's guard lends.
pub trait IoDoors: ReadDoors {}

/// TakesShort is what takes a lender, short or long: an entry's whole `Io` or a registration's view of it, never a
/// short lender's own view, under which no other lender is taken.
pub trait TakesShort: IoDoors {}

impl sealed::Sealed for Io {
    fn parts(&mut self) -> &mut Held {
        &mut self.held
    }
}

impl ReadDoors for Io {}

impl IoDoors for Io {}

impl TakesShort for Io {}

/// ShortIo is the view of an `Io` that a short lender's guard lends for its one bounded I/O step: it does file I/O and
/// takes locks under which no I/O runs, but takes no lender and checks no interrupt.
pub struct ShortIo<'a> {
    /// held is the lock scope of the `Io` it views.
    held: &'a mut Held,
}

impl<'a> ShortIo<'a> {
    /// new returns the view a short lender's guard lends.
    pub(crate) fn new(io: &'a mut impl TakesShort) -> ShortIo<'a> {
        ShortIo { held: io.parts() }
    }
}

impl Deref for ShortIo<'_> {
    type Target = Held;

    fn deref(&self) -> &Held {
        self.held
    }
}

impl DerefMut for ShortIo<'_> {
    fn deref_mut(&mut self) -> &mut Held {
        self.held
    }
}

impl sealed::Sealed for ShortIo<'_> {
    fn parts(&mut self) -> &mut Held {
        self.held
    }
}

impl ReadDoors for ShortIo<'_> {}

impl IoDoors for ShortIo<'_> {}

/// Registering is the view of an `Io` that a root's registration lends for as long as it lasts (design item 6): it
/// does file I/O, takes locks and short lenders, and reaches no door that takes the whole `Io`, so no user code runs
/// while a registration lasts. Nothing turns it back into an `Io`.
pub struct Registering<'a> {
    /// held is the lock scope of the `Io` it views.
    held: &'a mut Held,
}

impl<'a> Registering<'a> {
    /// new returns the view of an entry's `Io` that a registration's scope lends.
    pub fn new(io: &'a mut Io) -> Registering<'a> {
        Registering { held: sealed::Sealed::parts(io) }
    }
}

impl Deref for Registering<'_> {
    type Target = Held;

    fn deref(&self) -> &Held {
        self.held
    }
}

impl DerefMut for Registering<'_> {
    fn deref_mut(&mut self) -> &mut Held {
        self.held
    }
}

impl sealed::Sealed for Registering<'_> {
    fn parts(&mut self) -> &mut Held {
        self.held
    }
}

impl ReadDoors for Registering<'_> {}

impl IoDoors for Registering<'_> {}

impl TakesShort for Registering<'_> {}
