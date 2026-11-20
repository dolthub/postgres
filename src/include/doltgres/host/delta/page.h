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
 * The directory word of a page of the delta (host's delta/page.rs, design
 * item 14's S6): a slot's generation and its state in one u64, a page's
 * name, and the checks of an open, a read, an append, and the budget.
 */
#ifndef DOLTGRES_HOST_DELTA_PAGE_H
#define DOLTGRES_HOST_DELTA_PAGE_H

#include "doltgres/std/fmt.h"

/* HostDeltaPageState is what a slot of the page directory is to the processes that reach it. */
typedef enum HostDeltaPageState
{
	HOST_DELTA_PAGE_STATE_FREE = 0,
	HOST_DELTA_PAGE_STATE_OWNED = 1,
	HOST_DELTA_PAGE_STATE_LIVE = 2,
	HOST_DELTA_PAGE_STATE_RETIRED = 3,
	HOST_DELTA_PAGE_STATE_FREEING = 4,
	HOST_DELTA_PAGE_STATE_SPENT = 5,
} HostDeltaPageState;

/* HOST_DELTA_PAGE_SLOTS is the most slots a directory has, which the upper 31 bits of a name number. */
#define HOST_DELTA_PAGE_SLOTS (UINT64CONST(1) << 31)

/*
 * HOST_DELTA_PAGE_SLOT_BYTES is the memory of the delta each slot of the
 * directory stands for: the smallest page (bugs row 150).
 */
#define HOST_DELTA_PAGE_SLOT_BYTES (UINT64CONST(1) << 10)

/*
 * HOST_DELTA_PAGE_BLOCK is a block of the delta's region, which holds pages
 * of one size (bugs row 116): the smallest power of two that holds the
 * largest record, a blob's leaf with its version, so no record crosses a
 * block.
 */
#define HOST_DELTA_PAGE_BLOCK (UINT64CONST(64) << 10)

/* HOST_DELTA_PAGE_LARGEST is the most bytes a page's body takes, its words included: one block. */
#define HOST_DELTA_PAGE_LARGEST HOST_DELTA_PAGE_BLOCK

/*
 * HostDeltaPagePage is a page's name: its slot's number in the directory,
 * then the slot's generation while the page is in it. A name read from a word
 * may name no page, which the open refuses.
 */
typedef struct HostDeltaPagePage
{
	uint64		f0;
} HostDeltaPagePage;

/*
 * HostDeltaPagePhase is what an open finds a slot to be: a page or record a
 * hold may reach now, one retired, or neither.
 */
typedef enum HostDeltaPagePhase
{
	HOST_DELTA_PAGE_PHASE_CURRENT,
	HOST_DELTA_PAGE_PHASE_RETIRED,
	HOST_DELTA_PAGE_PHASE_NEITHER,
} HostDeltaPagePhase;

/* HostDeltaPageSeen is what one load of a slot's word shows an open: its generation and its phase. */
typedef struct HostDeltaPageSeen
{
	uint64		generation;
	HostDeltaPagePhase phase;
} HostDeltaPageSeen;

/* HostDeltaPageSize is a page's size: the capacity of its bytes and the count of its words. */
typedef struct HostDeltaPageSize
{
	uint32		bytes;
	uint32		words;
} HostDeltaPageSize;

extern HostDeltaPageState host_delta_page_state(uint64 word);
extern uint64 host_delta_page_generation(uint64 word);
extern uint64 host_delta_page_slots(uint64 delta_memory);
extern uint64 host_delta_page_directory_slots(uint64 delta_memory);
extern uint64 host_delta_page_blocks(uint64 delta_memory);
extern HostDeltaPagePage host_delta_page_Page_of(uint32 number, uint64 word);
extern HostDeltaPagePage host_delta_page_Page_named(uint64 value);
extern uint64 host_delta_page_Page_value(HostDeltaPagePage self);
extern uint64 host_delta_page_Page_number(HostDeltaPagePage self);
extern uint64 host_delta_page_Page_generation(HostDeltaPagePage self);
extern bool host_delta_page_Page_names(HostDeltaPagePage self, uint64 word);
extern bool host_delta_page_claim(uint64 word, uint64 *result);
extern uint64 host_delta_page_link(uint64 word);
extern bool host_delta_page_retire(uint64 word, HostDeltaPagePage name, uint64 *result);
extern bool host_delta_page_passed(uint64 stamp, uint64 oldest);
extern bool host_delta_page_reap(uint64 word, uint64 *result);
extern uint64 host_delta_page_released(uint64 word);
extern HostDeltaPageSeen host_delta_page_seen(uint64 word);
extern bool host_delta_page_admits(HostDeltaPageSeen before, uint64 stamp,
								   HostDeltaPageSeen after, uint64 generation,
								   uint64 epoch);
extern uint64 host_delta_page_Size_body(HostDeltaPageSize self);
extern bool host_delta_page_Size_allocates(HostDeltaPageSize self, uint64 delta_memory);
extern bool host_delta_page_lends(uint64 mark, uint64 offset, uint64 len);
extern bool host_delta_page_appended(uint64 mark, uint64 len, uint64 capacity,
									 uint64 *result);
extern bool host_delta_page_State_Debug_fmt(const void *self, StdFormatter *f);
extern bool host_delta_page_Page_Debug_fmt(const void *self, StdFormatter *f);
extern bool host_delta_page_Phase_Debug_fmt(const void *self, StdFormatter *f);
extern bool host_delta_page_Seen_Debug_fmt(const void *self, StdFormatter *f);
extern bool host_delta_page_Size_Debug_fmt(const void *self, StdFormatter *f);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_delta_page_tests_a_word_holds_a_state_and_a_generation(void);
extern void test_host_delta_page_tests_a_name_is_a_number_and_a_generation(void);
extern void test_host_delta_page_tests_each_transition_takes_only_its_own_state(void);
extern void test_host_delta_page_tests_a_released_slot_moves_on_and_its_last_generation_is_spent(void);
extern void test_host_delta_page_tests_a_stamp_is_passed_by_a_later_oldest_epoch(void);
extern void test_host_delta_page_tests_an_open_reaches_a_page_a_hold_may_reach(void);
extern void test_host_delta_page_tests_an_open_refuses_a_slot_remade_between_its_loads(void);
extern void test_host_delta_page_tests_a_size_is_a_body_no_larger_than_a_block(void);
extern void test_host_delta_page_tests_reads_end_at_the_mark_and_appends_at_the_capacity(void);
#endif

#endif							/* DOLTGRES_HOST_DELTA_PAGE_H */
