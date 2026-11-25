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

//! Backend-local state (design items 6 and 14): a static whose value each backend keeps for itself. In the server a
//! backend is a process, so its value is the static's own; the simulation runs several backends as actors in one
//! process, and the host names the running actor, whose value is its own.

use std::collections::BTreeMap;
use std::ops::Deref;
use std::sync::{Arc, OnceLock, PoisonError, RwLock};

/// BackendLocal is a value each backend keeps for itself, its type's default until the backend first changes it.
pub struct BackendLocal<T> {
    /// process is the value of the process's one backend, the server's.
    process: OnceLock<T>,
    /// actors are the values of the simulation's actors, by the actor the host names.
    actors: RwLock<BTreeMap<u64, Arc<T>>>,
}

/// Local is a backend's value of a `BackendLocal`.
pub enum Local<'a, T> {
    /// Process is the process's one backend's value.
    Process(&'a T),
    /// Actor is an actor's value.
    Actor(Arc<T>),
}

impl<T> Deref for Local<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        match self {
            Local::Process(value) => value,
            Local::Actor(value) => value,
        }
    }
}

impl<T> BackendLocal<T> {
    /// new returns a static's backend-local value.
    pub const fn new() -> BackendLocal<T> {
        BackendLocal { process: OnceLock::new(), actors: RwLock::new(BTreeMap::new()) }
    }
}

impl<T> Default for BackendLocal<T> {
    fn default() -> BackendLocal<T> {
        BackendLocal::new()
    }
}

impl<T: Default> BackendLocal<T> {
    /// get returns the running backend's value. An actor's value is cloned out of the map, since it must outlive the
    /// map's lock, whose read guard held across the value's use would deadlock a nested get that inserts.
    pub fn get(&self) -> Local<'_, T> {
        let Some(actor) = crate::current().actor() else {
            return Local::Process(self.process.get_or_init(T::default));
        };
        let found = self.actors.read().unwrap_or_else(PoisonError::into_inner).get(&actor).cloned();
        let value = match found {
            Some(value) => value,
            None => self.actors.write().unwrap_or_else(PoisonError::into_inner).entry(actor).or_default().clone(),
        };
        Local::Actor(value)
    }

    /// existing returns the running backend's value if it made one, without making it, which takes no lock that waits
    /// and allocates nothing, for a release that runs inside an abort: an actor's map, which no actor writes while
    /// another runs, is read only if no writer holds it.
    pub fn existing(&self) -> Option<Local<'_, T>> {
        let Some(actor) = crate::HOST.get().and_then(|host| host.actor()) else {
            let value = self.process.get()?;
            return Some(Local::Process(value));
        };
        let actors = self.actors.try_read().ok()?;
        let value = Arc::clone(actors.get(&actor)?);
        Some(Local::Actor(value))
    }
}
