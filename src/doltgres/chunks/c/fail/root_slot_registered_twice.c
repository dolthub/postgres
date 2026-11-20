/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * root_slot_registered_twice: a root slot lent to a second pin while the
 * first lasts is refused, as the Rust's second &mut borrow of the slot is,
 * since the first pin releases the slot's capability and the second finds it
 * not held.
 */
#include "postgres.h"

#include "doltgres/chunks/packs/roots.h"

/* root_slot_registered_twice_case pins two roots through one slot. */
extern void root_slot_registered_twice_case(const ChunksPacksBackendPacks *packs, HostIoTakesShort io,
											ChunksPacksRootsRootSlots *slots, ChunksAddressAddress address,
											ChunksTrailerLocator locator);

void
root_slot_registered_twice_case(const ChunksPacksBackendPacks *packs, HostIoTakesShort io,
								ChunksPacksRootsRootSlots *slots, ChunksAddressAddress address,
								ChunksTrailerLocator locator)
{
	ChunksPacksRootsRootSlot *slot;
	ChunksPacksRootsPin first;
	ChunksPacksRootsPin second;
	ChunksErrorError error;

	chunks_packs_roots_RootSlots_one(slots, &slot);
	(void) chunks_packs_roots_Packs_pin(packs, io, slot, address, locator, &first, &error);
	(void) chunks_packs_roots_Packs_pin(packs, io, slot, address, locator, &second, &error);
}
