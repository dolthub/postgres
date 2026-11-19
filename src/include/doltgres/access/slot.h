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
 * A slot's keyed layout (access' slot.rs), the forms of it other crates'
 * ports call before access's port defines them.
 */
#ifndef DOLTGRES_ACCESS_SLOT_H
#define DOLTGRES_ACCESS_SLOT_H

#include "doltgres/access/key.h"

/*
 * access_TupleUsizeOrderKeyType_t is a (usize, Order, KeyType): a key column's
 * position, its order, and its key type.
 */
typedef struct access_TupleUsizeOrderKeyType_t
{
	size_t		f0;
	EncodingKeyOrder f1;
	AccessKeyKeyType f2;
} access_TupleUsizeOrderKeyType_t;

extern void access_slot_keyed(const access_TupleUsizeOrderKeyType_t *key, size_t key_len, StdVec *result);

#endif							/* DOLTGRES_ACCESS_SLOT_H */
