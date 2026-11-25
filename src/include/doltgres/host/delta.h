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
 * The delta's pages (host's delta.rs, design item 14's S6): the memory of
 * the delta layer as pages of bytes written once and of words, which every
 * backend shares.
 */
/* lib.h is the umbrella, whose order of its modules' parts holds whichever header comes first. */
#include "doltgres/host/lib.h"

#ifndef DOLTGRES_HOST_DELTA_H
#define DOLTGRES_HOST_DELTA_H

#include "doltgres/host/delta/page.h"

/* HostDeltaWordOpTag is WordOp's variants. */
typedef enum HostDeltaWordOpTag
{
	HOST_DELTA_WORD_OP_LOAD,
	HOST_DELTA_WORD_OP_STORE,
	HOST_DELTA_WORD_OP_EXCHANGE,
	HOST_DELTA_WORD_OP_ADD,
} HostDeltaWordOpTag;

/* HostDeltaWordOp is an operation on one of a page's words. */
typedef struct HostDeltaWordOp
{
	HostDeltaWordOpTag tag;
	union
	{
		uint64		store;
		struct
		{
			uint64		f0;
			uint64		f1;
		}			exchange;
		uint64		add;
	}			u;
} HostDeltaWordOp;

/* HostDeltaLeaving is a record of an aged kind that a retirement takes with its pages: its shape's id and its location. */
typedef struct HostDeltaLeaving
{
	uint64		id;
	uint64		location;
} HostDeltaLeaving;

/*
 * HostDeltaFull is the budget's answer to an allocation the delta can't give
 * now: the count of frees the allocation loaded before it tried, which the
 * wait takes. Only a refusal makes one, and it is neither copied nor made by
 * a crate.
 */
typedef struct HostDeltaFull
{
	uint64		frees;
} HostDeltaFull;

/* The modules whose types name these, then the module's functions, which take lib.h's types. */
#include "doltgres/host/delta/directory.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/iter.h"

/*
 * HostDeltaHold is a reader of the fork-wide epochs for the entry that took
 * it: a page retired after it began isn't freed until it ends, and one
 * retired before it began isn't opened under it.
 */
typedef struct HostDeltaHold
{
	HostHost	host;
	uint64		epoch;
} HostDeltaHold;

/* HostDeltaPageRef is a page opened under a hold: the one way to its bytes, its words, its appends, and its seal. */
typedef struct HostDeltaPageRef
{
	HostHost	host;
	HostDeltaDirectoryOpened opened;
} HostDeltaPageRef;

/*
 * HostDeltaLent is a range of a page's bytes lent for as long as the hold
 * that opened the page: the server's is the page's own memory, which is never
 * written again below the mark, and the simulation's is a copy.
 */
typedef struct HostDeltaLent
{
	HostCacheBytes bytes;
} HostDeltaLent;

/* HostDeltaWord is one of the words of a page opened under a hold, which changes after the page is published. */
typedef struct HostDeltaWord
{
	HostHost	host;
	uint64		body;
	uint64		index;
} HostDeltaWord;

/* HostDeltaNamesTakesOps is names::Takes' table: a word that stores a page's name. */
typedef struct HostDeltaNamesTakesOps
{
	void		(*take) (void *self, uint64 name);
} HostDeltaNamesTakesOps;

/* HostDeltaNamesOps is Names' table, its supertrait's alone. */
typedef struct HostDeltaNamesOps
{
	HostDeltaNamesTakesOps takes;
} HostDeltaNamesOps;

/* HostDeltaNames is a word that takes a page's name at the page's link. */
typedef struct HostDeltaNames
{
	const HostDeltaNamesOps *ops;
	void	   *self;
} HostDeltaNames;

/*
 * HostDeltaOwned is a page its allocator owns: no open reaches it, so its
 * bytes and words are its owner's alone until its link. One dropped unlinked
 * gives its slot and its memory back, and none outlives the entry that
 * allocated it.
 */
typedef struct HostDeltaOwned
{
	HostHost	host;
	HostDeltaPagePage name;
} HostDeltaOwned;

/*
 * HostDeltaUnlinked is a record of an aged kind its maker owns: nothing names
 * it, so its parts are its maker's alone until its link. One dropped unlinked
 * gives its slot back at once, which counts as a free, and none outlives the
 * entry that made it.
 */
typedef struct HostDeltaUnlinked
{
	HostHost	host;
	uint64		place;
} HostDeltaUnlinked;

/*
 * HostDeltaMade is a record of an aged shape its maker owns until its link:
 * its location, its atomics, and the words that take a name, and no lock,
 * since nothing that takes a lock may reach a record no word names.
 */
typedef struct HostDeltaMade
{
	HostDeltaUnlinked unlinked;
	uint64		location;
} HostDeltaMade;

/* host_delta_With_own_t is an element of With's own: a page and its lock's index. */
typedef struct host_delta_With_own_t
{
	HostDeltaOwned f0;
	uint64		f1;
} host_delta_With_own_t;

/* host_delta_With_pages_t is an element of With's pages: a page and its owner. */
typedef struct host_delta_With_pages_t
{
	HostDeltaOwned f0;
	HostDeltaDirectoryOwner f1;
} host_delta_With_pages_t;

/*
 * HostDeltaWith is what a record's link makes live with it, gathered before
 * the link: pages the record's own locks will own, and unlinked records under
 * it with the pages their locks will own. So a page goes live only with an
 * owner that the same link publishes.
 */
typedef struct HostDeltaWith
{
	/*
	 * own are the pages the linked record's locks will own, each with its
	 * lock's index.
	 */
	StdVec		own;
	/* pages are the pages the records under it will own, each with its owner. */
	StdVec		pages;
	StdVec		records;
} HostDeltaWith;

/* HostDeltaRetireItem is retire's iterator's item, a Result<Page, Error>: a name, or an error. */
typedef struct HostDeltaRetireItem
{
	bool		is_ok;
	HostDeltaPagePage ok;
	HostError	err;
} HostDeltaRetireItem;

/* The words' tables of Names. */
extern const HostDeltaNamesOps HOST_DELTA_WORD_NAMES_OPS;
extern const HostDeltaNamesOps HOST_DELTA_NAMED_NAMES_OPS;
extern const HostDeltaNamesOps HOST_DELTA_PAST_NAMES_OPS;
extern const HostDeltaNamesOps HOST_DELTA_RUN_WORD_NAMES_OPS;

extern HostDeltaLeaving host_delta_Leaving_of(const HostSharedAgedOps *s, uint64 location);
#ifdef DOLTGRES_HOST_TESTING
extern HostDeltaLeaving host_delta_Leaving_raw(uint64 id, uint64 location);
#endif
extern bool host_delta_hold(HostIoIo *io, HostDeltaHold *result, HostError *error);
extern bool host_delta_Hold_page(const HostDeltaHold *self, HostDeltaPagePage name,
								 HostDeltaPageRef *result, HostError *error);
extern bool host_delta_Hold_record(const HostDeltaHold *self, const HostSharedAgedOps *s,
								   uint64 location, HostSharedRecord *result, HostError *error);
extern void host_delta_Hold_drop(HostDeltaHold *self);
extern bool host_delta_PageRef_bytes(const HostDeltaPageRef *self, uint64 offset, uint64 len,
									 HostDeltaLent *result, HostError *error);
extern uint64 host_delta_PageRef_room(const HostDeltaPageRef *self);
extern uint64 host_delta_PageRef_filled(const HostDeltaPageRef *self);
extern uint64 host_delta_PageRef_words(const HostDeltaPageRef *self);
extern bool host_delta_PageRef_word(const HostDeltaPageRef *self, uint64 index,
									HostDeltaWord *result, HostError *error);
extern bool host_delta_PageRef_append(const HostDeltaPageRef *self, const HostSharedShapeOps *s,
									  size_t l, HostSharedGuard *guard, const uint8 *record,
									  size_t record_len, uint32 *result, HostError *error);
extern bool host_delta_PageRef_seal(const HostDeltaPageRef *self, const HostSharedShapeOps *s,
									size_t l, HostSharedGuard *guard, HostError *error);
extern StdBytes host_delta_Lent_Deref_deref(const HostDeltaLent *self);
extern void host_delta_Lent_drop(HostDeltaLent *self);
extern uint64 host_delta_Word_load(const HostDeltaWord *self);
extern void host_delta_Word_store(const HostDeltaWord *self, uint64 value);
extern uint64 host_delta_Word_compare_exchange(const HostDeltaWord *self, uint64 expected,
											   uint64 value);
extern uint64 host_delta_Word_fetch_add(const HostDeltaWord *self, uint64 value);
extern HostDeltaPagePage host_delta_Owned_name(const HostDeltaOwned *self);
extern bool host_delta_Owned_append(HostDeltaOwned *self, const uint8 *record, size_t record_len,
									uint32 *result, HostError *error);
extern bool host_delta_Owned_store(HostDeltaOwned *self, uint64 index, uint64 value,
								   HostError *error);
extern HostDeltaPagePage host_delta_Owned_link(HostDeltaOwned *self, const HostSharedShapeOps *s,
											   size_t l, HostSharedGuard *guard,
											   HostDeltaNames word);
extern void host_delta_Owned_drop(void *self);
extern void host_delta_Unlinked_drop(void *self);
extern bool host_delta_make(HostIoIo *io, const HostSharedAgedOps *s, bool *result_ok,
							HostDeltaMade *result_made, HostDeltaFull *result_full,
							HostError *error);
extern uint64 host_delta_Made_location(const HostDeltaMade *self);
extern uint64 host_delta_Made_atomic(const HostDeltaMade *self, size_t a, HostSharedOp op);
extern HostSharedNamed host_delta_Made_names(const HostDeltaMade *self, size_t a);
extern uint64 host_delta_Made_link(HostDeltaMade *self, const HostSharedAgedOps *s,
								   HostDeltaNames word, HostDeltaWith *with);
extern void host_delta_Made_drop(HostDeltaMade *self);
extern HostDeltaWith host_delta_With_Default_default(void);
extern bool host_delta_With_page(HostDeltaWith *self, size_t l, HostDeltaOwned *page,
								 HostError *error);
extern bool host_delta_With_record(HostDeltaWith *self, const HostSharedAgedOps *r, size_t l,
								   HostDeltaMade *made, StdIterator pages, HostError *error);
extern void host_delta_With_drop(HostDeltaWith *self);
extern bool host_delta_alloc(HostIoIo *io, const HostDeltaPageSize *sizes, size_t sizes_len,
							 bool *result_ok, StdVec *result_owned, HostDeltaFull *result_full,
							 HostError *error);
extern bool host_delta_alloc_replacing(HostIoIo *io, HostDeltaPageSize size, HostDeltaPagePage replaced,
									   bool *result_ok, HostDeltaOwned *result_owned, HostDeltaFull *result_full,
									   HostError *error);
extern bool host_delta_wait(HostIoIo *io, HostDeltaFull *full, HostError *error);
extern bool host_delta_retire(HostIoIo *io, const HostDeltaLeaving *records, size_t records_len,
							  StdIterator pages, HostError *error);
extern bool host_delta_packing(const uint64 *shapes, size_t shapes_len, StdVec *result, HostError *error);
extern void host_delta_advanced(uint64 *result_0, uint64 *result_1);
extern uint64 host_delta_used(void);
extern uint64 host_delta_budget(void);
extern void host_delta_seed(uint8 *result);

#endif							/* DOLTGRES_HOST_DELTA_H */
