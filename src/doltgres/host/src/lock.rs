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

//! The storage crates' locks over a backend's own state. A lock under which no file I/O runs borrows its taker's
//! `Held` for as long as its guard lives, so no file door compiles under it, and a lock held across I/O is taken only
//! with the whole `Io`, which its guard lends back, so it is never taken under one that does no I/O (L1). A backend
//! runs one thread, so a lock found held was taken by its own taker further up, a re-entry, which is refused as the
//! bug it is rather than waited for; state shared between backends takes shared memory's locks instead. A lock a panic
//! poisoned holds state the panic may have left half changed, so the next ordinary take rebuilds it, to its default,
//! which its owner fills again lazily (B5), and a drop's take, which must not rebuild, gives nothing (H2). The
//! mutexes are reader-writer locks taken only exclusively, since std's mutex allocates its lock at the first take on
//! macOS, which a drop's take must not, and its reader-writer lock allocates on no platform.

use std::fmt::{self, Write as _};
use std::ops::{Deref, DerefMut};
use std::panic::Location;
use std::sync::{RwLock, RwLockReadGuard, RwLockWriteGuard, TryLockError, TryLockResult};

use crate::io::{Held, InDrop, ShortIo, TakesShort};

/// Declared shows where a lock is declared, its file with `/` between folders on every platform, so a refusal reads
/// the same everywhere.
struct Declared<'a>(&'a Location<'static>);

impl fmt::Display for Declared<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for c in self.0.file().chars() {
            f.write_char(if c == '\\' { '/' } else { c })?;
        }
        write!(f, ":{}:{}", self.0.line(), self.0.column())
    }
}

/// held_already fails the take of a lock found held, naming the lock by where it is declared: in the server its own
/// backend holds it, a re-entry, and under the simulation's actors another actor may, which is state shared between
/// backends outside shared memory.
fn held_already(at: &Location<'static>) -> ! {
    let at = Declared(at);
    match crate::acting() {
        false => panic!("a backend took the storage lock declared at {at}, which it already holds"),
        true => panic!(
            "an actor took the storage lock declared at {at}, which it or another actor holds: state shared between \
             backends outside shared memory, or a re-entry"
        ),
    }
}

/// taken returns a lock's guard, refusing a lock its backend already holds, and rebuilding the state of one a panic
/// poisoned to its default once `clear` clears the poison.
fn taken<G: DerefMut<Target = T>, T: Default>(
    result: TryLockResult<G>,
    clear: impl FnOnce(),
    at: &Location<'static>,
) -> G {
    match result {
        Ok(guard) => guard,
        Err(TryLockError::WouldBlock) => held_already(at),
        Err(TryLockError::Poisoned(poisoned)) => {
            let mut guard = poisoned.into_inner();
            *guard = T::default();
            clear();
            guard
        }
    }
}

/// shared returns a reader's guard, refusing a lock its backend holds exclusively; its taker rebuilt the state of a
/// poisoned one under a write first, so a poisoned result here is a new poisoning's, already rebuilt.
fn shared<G>(result: TryLockResult<G>, at: &Location<'static>) -> G {
    match result {
        Ok(guard) => guard,
        Err(TryLockError::WouldBlock) => held_already(at),
        Err(TryLockError::Poisoned(poisoned)) => poisoned.into_inner(),
    }
}

/// IoMutex is a mutex under which no file I/O runs.
#[derive(Debug)]
pub struct IoMutex<T> {
    lock: RwLock<T>,
    /// at is where the lock is declared, which a take that finds it held names.
    at: &'static Location<'static>,
}

impl<T> IoMutex<T> {
    /// new returns a mutex holding the value.
    #[track_caller]
    pub const fn new(value: T) -> IoMutex<T> {
        IoMutex { lock: RwLock::new(value), at: Location::caller() }
    }

    /// lock_in_drop takes the mutex for a drop, giving nothing when a frame the drop unwinds through holds it or a panic
    /// poisoned it.
    pub fn lock_in_drop<'a>(&'a self, _: &'a mut InDrop) -> Option<RwLockWriteGuard<'a, T>> {
        self.lock.try_write().ok()
    }
}

impl<T: Default> Default for IoMutex<T> {
    #[track_caller]
    fn default() -> IoMutex<T> {
        IoMutex::new(T::default())
    }
}

impl<T: Default> IoMutex<T> {
    /// lock takes the mutex, lending the taker's `Held` to the guard.
    pub fn lock<'a>(&'a self, held: &'a mut Held) -> Locked<'a, RwLockWriteGuard<'a, T>> {
        crate::yield_point();
        Locked { guard: taken(self.lock.try_write(), || self.lock.clear_poison(), self.at), held }
    }
}

/// IoRwLock is a reader-writer lock under which no file I/O runs.
#[derive(Debug)]
pub struct IoRwLock<T> {
    lock: RwLock<T>,
    /// at is where the lock is declared, which a take that finds it held names.
    at: &'static Location<'static>,
}

impl<T> IoRwLock<T> {
    /// new returns a lock holding the value.
    #[track_caller]
    pub const fn new(value: T) -> IoRwLock<T> {
        IoRwLock { lock: RwLock::new(value), at: Location::caller() }
    }
}

impl<T: Default> Default for IoRwLock<T> {
    #[track_caller]
    fn default() -> IoRwLock<T> {
        IoRwLock::new(T::default())
    }
}

impl<T: Default> IoRwLock<T> {
    /// read takes the lock shared, lending the taker's `Held` to the guard. A poisoned lock is rebuilt under a write,
    /// since a shared guard can't change its state.
    pub fn read<'a>(&'a self, held: &'a mut Held) -> Locked<'a, RwLockReadGuard<'a, T>> {
        crate::yield_point();
        if self.lock.is_poisoned() {
            drop(taken(self.lock.try_write(), || self.lock.clear_poison(), self.at));
        }
        Locked { guard: shared(self.lock.try_read(), self.at), held }
    }

    /// write takes the lock exclusively, lending the taker's `Held` to the guard.
    pub fn write<'a>(&'a self, held: &'a mut Held) -> Locked<'a, RwLockWriteGuard<'a, T>> {
        crate::yield_point();
        Locked { guard: taken(self.lock.try_write(), || self.lock.clear_poison(), self.at), held }
    }
}

/// Locked is the guard of a lock under which no file I/O runs, which holds its taker's `Held` until it drops.
pub struct Locked<'a, G> {
    /// guard is the lock's own guard.
    guard: G,
    /// held is the taker's lock scope, which no file door takes.
    held: &'a mut Held,
}

impl<G> Locked<'_, G> {
    /// held lends the guard's `Held` on, to take a further lock under it.
    pub fn held(&mut self) -> &mut Held {
        self.held
    }
}

impl<G: DerefMut> Locked<'_, G> {
    /// split returns the locked value and the guard's `Held` together, to change the one under a further lock the other
    /// takes.
    pub fn split(&mut self) -> (&mut G::Target, &mut Held) {
        (&mut *self.guard, self.held)
    }
}

impl<G: Deref> Locked<'_, G> {}

impl<G: Deref> Deref for Locked<'_, G> {
    type Target = G::Target;

    fn deref(&self) -> &G::Target {
        &self.guard
    }
}

impl<G: DerefMut> DerefMut for Locked<'_, G> {
    fn deref_mut(&mut self) -> &mut G::Target {
        &mut self.guard
    }
}

/// LongMutex is a mutex held across file I/O, which its taker takes with the whole `Io`, so it is never taken under a
/// lock that does no I/O.
#[derive(Debug)]
pub struct LongMutex<T> {
    lock: RwLock<T>,
    /// at is where the lock is declared, which a take that finds it held names.
    at: &'static Location<'static>,
}

impl<T> LongMutex<T> {
    /// new returns a mutex holding the value.
    #[track_caller]
    pub const fn new(value: T) -> LongMutex<T> {
        LongMutex { lock: RwLock::new(value), at: Location::caller() }
    }
}

impl<T: Default> Default for LongMutex<T> {
    #[track_caller]
    fn default() -> LongMutex<T> {
        LongMutex::new(T::default())
    }
}

impl<T: Default> LongMutex<T> {
    /// lock takes the mutex, lending the taker's `Io`, or its registration's view of one, to the guard.
    pub fn lock<'a, I: TakesShort>(&'a self, io: &'a mut I) -> Lending<'a, I, RwLockWriteGuard<'a, T>> {
        crate::yield_point();
        Lending { guard: taken(self.lock.try_write(), || self.lock.clear_poison(), self.at), io }
    }
}

/// Lending is the guard of a lock held across file I/O, which lends its taker's capability back to the holder.
pub struct Lending<'a, I, G> {
    /// guard is the lock's own guard.
    guard: G,
    /// io is the taker's capability, which the guard lends back.
    io: &'a mut I,
}

impl<I, G> Lending<'_, I, G> {
    /// io lends the guard's capability on, for file I/O or a lock under it.
    pub fn io(&mut self) -> &mut I {
        self.io
    }
}

impl<I, G: DerefMut> Lending<'_, I, G> {
    /// split returns the locked value and the guard's capability together, to change the one with I/O the other
    /// does.
    pub fn split(&mut self) -> Parts<'_, G::Target, I> {
        Parts { value: &mut *self.guard, io: self.io }
    }
}

impl<I, G: Deref> Deref for Lending<'_, I, G> {
    type Target = G::Target;

    fn deref(&self) -> &G::Target {
        &self.guard
    }
}

impl<I, G: DerefMut> DerefMut for Lending<'_, I, G> {
    fn deref_mut(&mut self) -> &mut G::Target {
        &mut self.guard
    }
}

/// Parts is a lender's guarded value and the capability it lends, which `split` gives together, so that the one
/// changes with the I/O the other does.
pub struct Parts<'a, T: ?Sized, I> {
    /// value is the guarded value.
    pub value: &'a mut T,
    /// io is the capability the guard lends.
    pub io: &'a mut I,
}

/// ShortMutex is a mutex held across one bounded step of file I/O and nothing else: its guard lends a `ShortIo`, which
/// takes no other lender and checks no interrupt.
#[derive(Debug)]
pub struct ShortMutex<T> {
    lock: RwLock<T>,
    /// at is where the lock is declared, which a take that finds it held names.
    at: &'static Location<'static>,
}

impl<T> ShortMutex<T> {
    /// new returns a mutex holding the value.
    #[track_caller]
    pub const fn new(value: T) -> ShortMutex<T> {
        ShortMutex { lock: RwLock::new(value), at: Location::caller() }
    }
}

impl<T: Default> Default for ShortMutex<T> {
    #[track_caller]
    fn default() -> ShortMutex<T> {
        ShortMutex::new(T::default())
    }
}

impl<T: Default> ShortMutex<T> {
    /// lock takes the mutex, lending a view of the taker's `Io` that does file I/O for the step to the guard.
    pub fn lock<'a>(&'a self, io: &'a mut impl TakesShort) -> Short<'a, RwLockWriteGuard<'a, T>> {
        crate::yield_point();
        Short { guard: taken(self.lock.try_write(), || self.lock.clear_poison(), self.at), io: ShortIo::new(io) }
    }
}

/// Short is the guard of a short lender, which lends the view of its taker's `Io` that does its step's file I/O.
pub struct Short<'a, G> {
    /// guard is the lock's own guard.
    guard: G,
    /// io is the view of the taker's `Io` the guard lends.
    io: ShortIo<'a>,
}

impl<'a, G> Short<'a, G> {
    /// io lends the guard's view on, for the step's file I/O or a lock under which no I/O runs.
    pub fn io(&mut self) -> &mut ShortIo<'a> {
        &mut self.io
    }
}

impl<'a, G: DerefMut> Short<'a, G> {
    /// split returns the locked value and the guard's view together, to change the one with the I/O the other does.
    pub fn split(&mut self) -> Parts<'_, G::Target, ShortIo<'a>> {
        Parts { value: &mut *self.guard, io: &mut self.io }
    }
}

impl<G: Deref> Deref for Short<'_, G> {
    type Target = G::Target;

    fn deref(&self) -> &G::Target {
        &self.guard
    }
}

impl<G: DerefMut> DerefMut for Short<'_, G> {
    fn deref_mut(&mut self) -> &mut G::Target {
        &mut self.guard
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// with_held runs a test's body with a lock scope of its own.
    fn with_held<R>(body: impl FnOnce(&mut Held) -> R) -> R {
        body(&mut Held::testing())
    }

    /// A lock under which no I/O runs gives its value through its guard, and lends the taker's `Held` on.
    #[test]
    fn a_guard_gives_the_value_and_lends_the_held() {
        let (outer, inner) = (IoMutex::new(1), IoMutex::new(2));
        with_held(|held| {
            let mut guard = outer.lock(held);
            let (value, held) = guard.split();
            *value += *inner.lock(held);
        });
        assert_eq!(with_held(|held| *outer.lock(held)), 3);
    }

    /// assert_refused checks that a body panics with a backend's refusal of a lock it holds, which names the lock's
    /// declaration with `/` between folders on every platform.
    fn assert_refused(body: impl FnOnce()) {
        let payload = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)).expect_err("the take is refused");
        let message = payload.downcast::<String>().expect("the refusal's message is formatted");
        assert!(message.starts_with("a backend took the storage lock declared at "), "{message}");
        assert!(message.ends_with(", which it already holds"), "{message}");
        assert!(!message.contains('\\'), "{message}");
    }

    /// A backend's second take of a mutex it holds is a re-entry, refused rather than waited for.
    #[test]
    fn a_second_take_of_a_held_mutex_is_refused() {
        let mutex = IoMutex::new(());
        assert_refused(|| {
            with_held(|first| {
                let _first = mutex.lock(first);
                with_held(|second| drop(mutex.lock(second)));
            });
        });
    }

    /// Readers share a reader-writer lock, and a writer under a reader is a re-entry, refused.
    #[test]
    fn a_write_under_a_read_is_refused() {
        let lock = IoRwLock::new(());
        assert_refused(|| {
            with_held(|first| {
                let _first = lock.read(first);
                with_held(|second| {
                    let _second = lock.read(second);
                    with_held(|third| drop(lock.write(third)));
                });
            });
        });
    }

    /// STATIC is backend-local state behind a storage lock in a static, as a crate keeps it.
    static STATIC: IoMutex<Vec<u8>> = IoMutex::new(Vec::new());

    /// A panic while a static storage lock is held poisons it, and the next take rebuilds its state to its default,
    /// which the owner fills again, rather than refusing every later entry.
    #[test]
    fn a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take() {
        with_held(|held| STATIC.lock(held).push(1));
        let panicked = std::panic::catch_unwind(|| {
            with_held(|held| {
                STATIC.lock(held).push(2);
                let _guard = STATIC.lock(held);
                panic!("a step's bug while the lock is held");
            })
        });
        assert!(panicked.is_err());
        assert!(STATIC.lock.is_poisoned());
        with_held(|held| {
            let mut state = STATIC.lock(held);
            assert!(state.is_empty());
            state.push(3);
        });
        assert_eq!(with_held(|held| STATIC.lock(held).clone()), [3]);
    }

    /// Releasing drops a root from a list a lock holds, the way a pin's drop does.
    struct Releasing<'a>(&'a IoMutex<Vec<u8>>);

    impl Drop for Releasing<'_> {
        fn drop(&mut self) {
            if let Some(mut roots) = self.0.lock_in_drop(&mut InDrop::new()) {
                roots.pop();
            }
        }
    }

    /// A drop whose lock a frame it unwinds through holds gives up its work instead of panicking during the unwind,
    /// which would abort the process: the unwind's panic is the one caught, the drop gives nothing on the poisoned lock
    /// after it either, and the next ordinary take rebuilds the state.
    #[test]
    fn a_drop_under_an_unwind_through_its_lock_defers() {
        let roots = IoMutex::new(vec![1, 2]);
        drop(Releasing(&roots));
        assert_eq!(with_held(|held| roots.lock(held).clone()), [1]);
        let unwound = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            with_held(|held| {
                let _guard = roots.lock(held);
                let _releasing = Releasing(&roots);
                panic!("a step's error while the lock is held");
            })
        }));
        assert!(unwound.is_err());
        drop(Releasing(&roots));
        assert!(with_held(|held| roots.lock(held).is_empty()));
    }
}
