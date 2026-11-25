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
 * Every LWLock tranche the fork registers, in registration order:
 * DOLTGRES_TRANCHE(id, name), where id names the DoltgresTranche member and
 * name is what wait events show. Included with DOLTGRES_TRANCHE defined, as
 * lwlocklist.h is, so the fork's count of its tranches is this list's length.
 */
DOLTGRES_TRANCHE(SHMEM, "versioned shared memory")
DOLTGRES_TRANCHE(RECORDS_INDEX, "versioned pool index")
DOLTGRES_TRANCHE(CACHE, "versioned node cache")
DOLTGRES_TRANCHE(KEYS, "versioned key")
DOLTGRES_TRANCHE(COLLECTION, "versioned collection")
DOLTGRES_TRANCHE(COLLECTION_ASKING, "versioned collection ask")
