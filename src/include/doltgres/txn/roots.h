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
 * A collection's roots in a repository's states and deltas (txn's
 * roots.rs). Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_ROOTS_H
#define DOLTGRES_TXN_ROOTS_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/std/vec.h"

/*
 * TxnRootsCaptured is Captured: what a collection captures in a repository's
 * states and deltas, its roots and the addresses of the chunks the delta of
 * chunks holds, which no pack holds, ordered and each once, as a BTreeSet
 * holds them.
 */
typedef struct TxnRootsCaptured
{
	/* roots is a Vec<Root>, each Root an (Address, Locator). */
	StdVec		roots;

	/*
	 * outside is a BTreeSet<Address>, a Vec<Address> in address order with
	 * each address once.
	 */
	StdVec		outside;
} TxnRootsCaptured;

extern void txn_roots_Captured_drop(TxnRootsCaptured *self);
extern bool txn_roots_roots(HostIoIo *io, uint32 database, HostLsn redo, TxnRootsCaptured *result,
							HostError *error);

#endif							/* DOLTGRES_TXN_ROOTS_H */
