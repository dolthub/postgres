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
 * The delta's memory in the server (shmem's delta.rs, design item 14's S6):
 * the page directory and the region of blocks.
 */
#ifndef DOLTGRES_SHMEM_DELTA_H
#define DOLTGRES_SHMEM_DELTA_H

#ifndef FRONTEND

#include "doltgres/pg/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/c_str.h"

/* SHMEM_DELTA_FIELDS is the number of a slot's words beside its generation-and-state word. */
#define SHMEM_DELTA_FIELDS ((size_t) 7)

/* SHMEM_DELTA_REGION_WORDS is the count of the region's words in the header. */
#define SHMEM_DELTA_REGION_WORDS ((size_t) 11)

/* SHMEM_DELTA_BLOCK_WORDS is the count of a block's words in the directory. */
#define SHMEM_DELTA_BLOCK_WORDS ((size_t) 3)

/* SHMEM_DELTA_HINTED is the count of slots each word of the free hint has a bit for (bugs row 150). */
#define SHMEM_DELTA_HINTED ((uint32) 64)

extern size_t shmem_delta_hints(size_t slots);
extern bool shmem_delta_request(const PgMayRaise *raise, size_t slots, size_t blocks, size_t bytes,
								PgPgError *error);
extern bool shmem_delta_initialize(const PgMayRaise *raise, size_t slots, size_t blocks, PgPgError *error);
extern uint32 shmem_delta_blocks(void);
extern uint64 shmem_delta_region(size_t which);
extern void shmem_delta_set_region(size_t which, uint64 value);
extern uint64 shmem_delta_block(uint32 at, size_t which);
extern void shmem_delta_set_block(uint32 at, size_t which, uint64 value);
extern void shmem_delta_lock_region(void);
extern void shmem_delta_unlock_region(void);
extern uint32 shmem_delta_slots(void);
extern uint64 shmem_delta_word(uint32 at);
extern bool shmem_delta_swap(uint32 at, uint64 loaded, uint64 word);
extern uint64 shmem_delta_load(uint32 at, size_t which);
extern void shmem_delta_store(uint32 at, size_t which, uint64 value);
extern uint64 shmem_delta_hint(uint32 word);
extern void shmem_delta_hint_set(uint32 slot);
extern void shmem_delta_hint_clear(uint32 slot);
extern uint64 shmem_delta_spent(bool over);
extern void shmem_delta_spend(bool over);
extern uint64 shmem_delta_frees(void);
extern void shmem_delta_freed(void);
extern uint64 shmem_delta_left(void);
extern void shmem_delta_leave(void);
extern void shmem_delta_settle(uint64 loaded);
extern uint64 shmem_delta_kept(void);
extern bool shmem_delta_kept_swap(uint64 loaded, uint64 word);
extern uint64 shmem_delta_waiting(void);
extern uint64 shmem_delta_overshoot(void);
extern bool shmem_delta_overshoot_swap(uint64 loaded, uint64 word);
extern void shmem_delta_count_waiter(bool add);
extern uint32 shmem_delta_hand(void);
extern uint64 shmem_delta_retired(void);
extern bool shmem_delta_retired_swap(uint64 loaded, uint64 word);
extern uint32 shmem_delta_link(uint32 slot);
extern void shmem_delta_set_link(uint32 slot, uint32 value);
extern void shmem_delta_lock_pass(void);
extern void shmem_delta_unlock_pass(void);
extern bool shmem_delta_lock(const PgMayRaise *raise, PgPgError *error);
extern void shmem_delta_unlock(void);
extern bool shmem_delta_wait(const PgMayRaise *raise, uint64 seen, PgPgError *error);
extern void shmem_delta_zero(uint64 pointer, uint64 len);
extern void shmem_delta_ask_fold(void);
extern uint64 shmem_delta_fold_asked(void);
extern void shmem_delta_launcher_started(void);
extern bool shmem_delta_launcher_running(void);
extern void shmem_delta_refuse(const char *detail, size_t detail_len);
extern bool shmem_delta_refusal(StdCString *result);
extern void shmem_delta_write(uint64 pointer, uint64 at, const uint8 *bytes, size_t bytes_len);
extern const uint8 *shmem_delta_bytes(uint64 pointer, uint64 at, uint64 len, size_t *result_len);
extern uint64 shmem_delta_body_word(uint64 pointer, uint64 index, ShmemRecordsOp op);
extern void shmem_delta_body_store(uint64 pointer, uint64 index, uint64 value);
extern void shmem_delta_stop(const char *point);

#ifdef DOLTGRES_TEST_SHMEM
extern void test_shmem_delta_tests_the_directory_is_a_header_and_slots_of_a_cache_line(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_DELTA_H */
