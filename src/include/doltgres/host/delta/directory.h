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
 * The delta's page directory as a host keeps it (host's delta/directory.rs,
 * design item 14's S6): what each host gives of its slots, its header, its
 * bodies, and the epochs, and the sequences over them, written once so the
 * server's host and the simulation's can't differ in them.
 */
/* lib.h is the umbrella, whose order of its modules' parts holds whichever header comes first. */
#include "doltgres/host/lib.h"

#ifndef DOLTGRES_HOST_DELTA_DIRECTORY_H
#define DOLTGRES_HOST_DELTA_DIRECTORY_H

#include "doltgres/host/delta/blocks.h"
#include "doltgres/host/delta/page.h"

/* HostDeltaDirectoryField is one of a slot's words beside its generation-and-state word. */
typedef enum HostDeltaDirectoryField
{
	HOST_DELTA_DIRECTORY_FIELD_BODY,
	HOST_DELTA_DIRECTORY_FIELD_SIZES,
	HOST_DELTA_DIRECTORY_FIELD_MARK,
	HOST_DELTA_DIRECTORY_FIELD_OWNER,
	HOST_DELTA_DIRECTORY_FIELD_LOCK,
	HOST_DELTA_DIRECTORY_FIELD_STAMP,
	HOST_DELTA_DIRECTORY_FIELD_SHAPE,
} HostDeltaDirectoryField;

/* HostDeltaDirectoryPoint is a place inside a sequence where the fork's tests hold a process. */
typedef enum HostDeltaDirectoryPoint
{
	HOST_DELTA_DIRECTORY_POINT_OPENING,
	HOST_DELTA_DIRECTORY_POINT_STAMPED,
	HOST_DELTA_DIRECTORY_POINT_CLAIMED,
	HOST_DELTA_DIRECTORY_POINT_PASSING,
	HOST_DELTA_DIRECTORY_POINT_WALKED,
	HOST_DELTA_DIRECTORY_POINT_STAMPING,
	HOST_DELTA_DIRECTORY_POINT_OLDEST,
	HOST_DELTA_DIRECTORY_POINT_WAITING,
	HOST_DELTA_DIRECTORY_POINT_HINTING,
} HostDeltaDirectoryPoint;

/*
 * HostDeltaDirectoryOwner is the lock that may append to a page: its record's
 * shape's id, its record's location, and its index among the record's locks.
 * A shape has one form and its locations are its own, so no two locks share
 * an owner.
 */
typedef struct HostDeltaDirectoryOwner
{
	uint64		shape;
	uint64		location;
	uint64		lock;
} HostDeltaDirectoryOwner;

/* HostDeltaDirectoryOpened is a page an open reached: its slot, its body's place, and its size. */
typedef struct HostDeltaDirectoryOpened
{
	uint32		slot;
	uint64		body;
	HostDeltaPageSize size;
} HostDeltaDirectoryOpened;

/* The module's trait and functions, which take lib.h's types, follow its include. */
#include "doltgres/host/lib.h"

/*
 * HostDeltaDirectoryDirectoryOps is the Directory trait's table: what a
 * host gives of its page directory and its region of blocks. Its associated
 * type Raise is the const void * its allocation lock takes.
 */
typedef struct HostDeltaDirectoryDirectoryOps
{
	uint32		(*slots) (void *self);
	uint64		(*budget) (void *self);
	uint64		(*word) (void *self, uint32 slot);
	bool		(*swap) (void *self, uint32 slot, uint64 loaded, uint64 word);
	uint64		(*load) (void *self, uint32 slot, HostDeltaDirectoryField field);
	void		(*store) (void *self, uint32 slot, HostDeltaDirectoryField field, uint64 value);
	uint32		(*blocks) (void *self);
	uint64		(*region) (void *self, HostDeltaBlocksRegion word);
	void		(*set_region) (void *self, HostDeltaBlocksRegion word, uint64 value);
	uint64		(*block) (void *self, uint32 block, HostDeltaBlocksBlock word);
	void		(*set_block) (void *self, uint32 block, HostDeltaBlocksBlock word, uint64 value);
	void		(*lock_region) (void *self);
	void		(*unlock_region) (void *self);
	uint64		(*frees) (void *self);
	void		(*freed) (void *self);
	uint64		(*left) (void *self);
	void		(*leave) (void *self);
	void		(*settle) (void *self, uint64 loaded);
	uint64		(*kept) (void *self);
	bool		(*kept_swap) (void *self, uint64 loaded, uint64 word);
	uint64		(*waiting) (void *self);
	void		(*count_waiter) (void *self, bool add);
	uint64		(*overshoot) (void *self);
	bool		(*overshoot_swap) (void *self, uint64 loaded, uint64 word);
	uint32		(*hand) (void *self);
	uint64		(*hint) (void *self, uint32 word);
	void		(*hint_set) (void *self, uint32 slot);
	void		(*hint_clear) (void *self, uint32 slot);
	uint64		(*spent) (void *self, bool over);
	void		(*spend) (void *self, bool over);
	bool		(*lock) (void *self, const void *raise, HostError *error);
	void		(*unlock) (void *self);
	void		(*zero) (void *self, uint64 body, HostDeltaPageSize size);
	void		(*write) (void *self, uint64 body, uint64 at, const uint8 *bytes, size_t bytes_len);
	void		(*set) (void *self, uint64 body, uint64 index, uint64 value);
	void		(*own) (void *self, HostDeltaPagePage name);
	void		(*disown) (void *self, HostDeltaPagePage name);
	uint64		(*retiring) (void *self);
	bool		(*retiring_now) (void *self, uint64 *result);
	void		(*retired) (void *self);
	void		(*unretired) (void *self);
	uint64		(*epoch) (void *self);
	uint64		(*oldest) (void *self);
	void		(*stop) (void *self, HostDeltaDirectoryPoint point);
	uint32		(*aged) (void *self);
	bool		(*aged_kind) (void *self, uint64 id, uint32 *result);
	uint32		(*records) (void *self, uint32 kind);
	uint64		(*record_word) (void *self, uint32 kind, uint32 index);
	bool		(*record_swap) (void *self, uint32 kind, uint32 index, uint64 loaded, uint64 word);
	uint64		(*stamp) (void *self, uint32 kind, uint32 index);
	void		(*set_stamp) (void *self, uint32 kind, uint32 index, uint64 stamp);
	uint32		(*record_hand) (void *self, uint32 kind);
	void		(*clear) (void *self, uint32 kind, uint32 index);
	void		(*made) (void *self, uint32 kind, uint32 index);
	void		(*unmade) (void *self, uint32 kind, uint32 index);
	uint64		(*retired_head) (void *self);
	bool		(*retired_swap) (void *self, uint64 loaded, uint64 word);
	uint32		(*link) (void *self, uint32 slot);
	void		(*set_link) (void *self, uint32 slot, uint32 value);
	uint64		(*record_retired_head) (void *self, uint32 kind);
	bool		(*record_retired_swap) (void *self, uint32 kind, uint64 loaded, uint64 word);
	uint64		(*record_link) (void *self, uint32 kind, uint32 index);
	void		(*set_record_link) (void *self, uint32 kind, uint32 index, uint64 value);
	void		(*lock_pass) (void *self);
	void		(*unlock_pass) (void *self);
} HostDeltaDirectoryDirectoryOps;

/* HOST_DELTA_DIRECTORY_HINTED is the count of slots each word of the free hint has a bit for. */
#define HOST_DELTA_DIRECTORY_HINTED 64

/* HostDeltaDirectoryDirectory is a host's page directory: its table and the host's value. */
typedef struct HostDeltaDirectoryDirectory
{
	const HostDeltaDirectoryDirectoryOps *ops;
	void	   *self;
} HostDeltaDirectoryDirectory;

extern bool host_delta_directory_alloc(HostDeltaDirectoryDirectory directory, const void *raise,
									   const HostDeltaPageSize *wanted, size_t wanted_len,
									   StdVec *names, bool *has_result, uint64 *result,
									   HostError *error);
extern bool host_delta_directory_alloc_replacing(HostDeltaDirectoryDirectory directory, const void *raise,
												 HostDeltaPageSize size, HostDeltaPagePage replaced, StdVec *names,
												 bool *has_result, uint64 *result, HostError *error);
extern void host_delta_directory_give_back(HostDeltaDirectoryDirectory directory,
										   HostDeltaPagePage name);
extern bool host_delta_directory_owned_append(HostDeltaDirectoryDirectory directory,
											  HostDeltaPagePage name, const uint8 *record,
											  size_t record_len, uint32 *result, HostError *error);
extern bool host_delta_directory_owned_store(HostDeltaDirectoryDirectory directory,
											 HostDeltaPagePage name, uint64 index, uint64 value,
											 HostError *error);
extern bool host_delta_directory_make(HostDeltaDirectoryDirectory directory, uint32 kind,
									  const char *name, size_t name_len, bool *result_ok,
									  uint32 *result_0, uint64 *result_1, uint64 *result_err,
									  HostError *error);
extern void host_delta_directory_unmake(HostDeltaDirectoryDirectory directory, uint32 kind,
										uint32 index);
extern void host_delta_directory_publish(HostDeltaDirectoryDirectory directory, uint32 kind,
										 uint32 index);
extern bool host_delta_directory_open_record(HostDeltaDirectoryDirectory directory, uint32 kind,
											 uint64 location, uint64 epoch, uint32 *result,
											 HostError *error);
extern void host_delta_directory_link(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
									  HostDeltaDirectoryOwner owner);
extern bool host_delta_directory_open(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
									  uint64 epoch, HostDeltaDirectoryOpened *result,
									  HostError *error);
extern bool host_delta_directory_readable(HostDeltaDirectoryDirectory directory,
										  const HostDeltaDirectoryOpened *opened, uint64 offset,
										  uint64 len, uint64 *result, HostError *error);
extern uint64 host_delta_directory_room(HostDeltaDirectoryDirectory directory,
										const HostDeltaDirectoryOpened *opened);
extern uint64 host_delta_directory_filled(HostDeltaDirectoryDirectory directory,
										  const HostDeltaDirectoryOpened *opened);
extern bool host_delta_directory_worded(HostDeltaPageSize size, uint64 index, HostError *error);
extern bool host_delta_directory_append(HostDeltaDirectoryDirectory directory,
										const HostDeltaDirectoryOpened *opened,
										HostDeltaDirectoryOwner owner, const uint8 *record,
										size_t record_len, uint32 *result, HostError *error);
extern bool host_delta_directory_seal(HostDeltaDirectoryDirectory directory,
									  const HostDeltaDirectoryOpened *opened,
									  HostDeltaDirectoryOwner owner, HostError *error);
extern void host_delta_directory_retire(HostDeltaDirectoryDirectory directory,
										const struct HostDeltaLeaving *records, size_t records_len,
										const HostDeltaPagePage *names, size_t names_len);
extern void host_delta_directory_pass(HostDeltaDirectoryDirectory directory);
extern void host_delta_directory_released(HostDeltaDirectoryDirectory directory, uint64 epoch);
extern void host_delta_directory_waiting(HostDeltaDirectoryDirectory directory);
extern void host_delta_directory_woken(HostDeltaDirectoryDirectory directory);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_delta_directory_tests_a_size_fits_its_word(void);
extern void test_host_delta_directory_tests_a_words_index_is_below_its_pages_count(void);
extern void test_host_delta_directory_tests_a_free_pass_loads_only_the_retirements(void);
extern void test_host_delta_directory_tests_a_refused_allocations_pass_loads_only_the_retirements(void);
#endif

#endif							/* DOLTGRES_HOST_DELTA_DIRECTORY_H */
