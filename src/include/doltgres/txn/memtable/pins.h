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
 * The pins of memtables a process reads (txn's memtable/pins.rs): each
 * process's slot of the memtable each of its trees pins.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_PINS_H
#define DOLTGRES_TXN_MEMTABLE_PINS_H

#include "doltgres/host/shared.h"

/* TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS is Pins' ProcSlots: each process's slot of the memtables it pins. */
extern const HostSharedProcSlotsOps TXN_MEMTABLE_PINS_PINS_PROC_SLOTS_OPS;

/* The words of a process's slot of pins, one for each kind of memtable: its rows', its free space map's, its value map's, a branch's chunks', and a working state's identity tree's. */
#define TXN_MEMTABLE_PINS_ROW_PIN 0
#define TXN_MEMTABLE_PINS_FREE_PIN 1
#define TXN_MEMTABLE_PINS_VALUES_PIN 2
#define TXN_MEMTABLE_PINS_CHUNK_PIN 3
#define TXN_MEMTABLE_PINS_IDENTITY_PIN 4

extern bool txn_memtable_pins_pinned(size_t word, uint64 memtable, bool *result, HostError *error);
extern void txn_memtable_pins_pin(const HostSharedMine *mine, size_t word, uint64 memtable);
extern void txn_memtable_pins_unpin(const HostSharedMine *mine, size_t word);

#endif							/* DOLTGRES_TXN_MEMTABLE_PINS_H */
