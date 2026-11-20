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
 * The directory word of a pooled record (host's shared/word.rs, design item
 * 14's S2): a slot's generation, its holders, its state, and its two marks
 * in one u64, with every transition a plain function from the word a host
 * loaded to the word it stores.
 */
#ifndef DOLTGRES_HOST_SHARED_WORD_H
#define DOLTGRES_HOST_SHARED_WORD_H

#include "doltgres/std/fmt.h"

/* HostSharedWordState is what a slot is to the processes that reach it. */
typedef enum HostSharedWordState
{
	HOST_SHARED_WORD_STATE_FREE = 0,
	HOST_SHARED_WORD_STATE_OWNED = 1,
	HOST_SHARED_WORD_STATE_PUBLISHED = 2,
	HOST_SHARED_WORD_STATE_CLAIMED = 3,
	HOST_SHARED_WORD_STATE_CONDEMNED = 4,
	HOST_SHARED_WORD_STATE_SPENT = 5,
} HostSharedWordState;

/* HOST_SHARED_WORD_HOLDERS_MAX is the most holders a word counts. */
#define HOST_SHARED_WORD_HOLDERS_MAX UINT64CONST(0xFFFFFF)

/* HostSharedWordLookTag is Look's variants. */
typedef enum HostSharedWordLookTag
{
	HOST_SHARED_WORD_LOOK_TAKE,
	HOST_SHARED_WORD_LOOK_THEIRS,
	HOST_SHARED_WORD_LOOK_USED,
} HostSharedWordLookTag;

/* HostSharedWordLook is what a reclaim's look at a record finds. */
typedef struct HostSharedWordLook
{
	HostSharedWordLookTag tag;
	union
	{
		uint64		take;
	}			u;
} HostSharedWordLook;

extern HostSharedWordState host_shared_word_state(uint64 word);
extern uint64 host_shared_word_holders(uint64 word);
extern uint64 host_shared_word_generation(uint64 word);
extern uint64 host_shared_word_location(uint32 slot, uint64 word);
extern uint64 host_shared_word_slot(uint64 location);
extern bool host_shared_word_live(uint64 word, uint64 location);
extern bool host_shared_word_published(uint64 word);
extern bool host_shared_word_swept(uint64 first, uint64 second);
extern bool host_shared_word_make(uint64 word, uint64 *result);
extern uint64 host_shared_word_publish(uint64 word);
extern bool host_shared_word_claim(uint64 word, uint64 location, uint64 *result);
extern bool host_shared_word_open(uint64 word, uint64 location, uint64 *result);
extern uint64 host_shared_word_close(uint64 word);
extern uint64 host_shared_word_free(uint64 word);
extern uint64 host_shared_word_give_up(uint64 word);
extern HostSharedWordLook host_shared_word_reclaim(uint64 word, uint64 location);
extern uint64 host_shared_word_reclaimed(uint64 taken);
extern uint64 host_shared_word_unreclaim(uint64 word);
extern bool host_shared_word_State_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_shared_word_Look_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_shared_word_Look_PartialEq_eq(const HostSharedWordLook *self,
											   const HostSharedWordLook *other);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_shared_word_tests_the_most_holders_fit_their_field(void);
extern void test_host_shared_word_tests_a_location_names_a_slot_and_a_generation(void);
extern void test_host_shared_word_tests_a_freed_record_is_gone_for_its_location(void);
extern void test_host_shared_word_tests_the_last_close_frees_a_condemned_record(void);
extern void test_host_shared_word_tests_a_dropped_owner_gives_its_record_up(void);
extern void test_host_shared_word_tests_a_reclaim_frees_only_a_record_not_opened_since(void);
extern void test_host_shared_word_tests_a_reclaim_does_not_free_a_record_claimed_since(void);
extern void test_host_shared_word_tests_a_reclaim_given_up_frees_a_condemned_record(void);
extern void test_host_shared_word_tests_a_slot_freed_at_its_last_generation_is_spent(void);
extern void test_host_shared_word_tests_a_sweep_lists_a_record_published_at_both_loads(void);
extern void test_host_shared_word_tests_a_record_is_live_until_its_slot_is_given_back(void);
#endif

#endif							/* DOLTGRES_HOST_SHARED_WORD_H */
