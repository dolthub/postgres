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
 * A record's lists (host's delta/list.rs, design item 14's S6): nine words
 * of the record, its first six entries, its root page's name, its depth and
 * reach, and its length, and past the first six a tree of 4 KiB pages of
 * words, each entry found by its index alone.
 */
#ifndef DOLTGRES_HOST_DELTA_LIST_H
#define DOLTGRES_HOST_DELTA_LIST_H

#include "doltgres/host/delta/page.h"

/* HOST_DELTA_LIST_WORDS is the number of a record's words a list takes. */
#define HOST_DELTA_LIST_WORDS 9

/* HOST_DELTA_LIST_INLINE is the number of a list's entries in its record. */
#define HOST_DELTA_LIST_INLINE UINT64CONST(6)

/* HOST_DELTA_LIST_FANOUT is the number of words of a list page, each an entry or the name of a page a level down. */
#define HOST_DELTA_LIST_FANOUT UINT64CONST(512)

/* HOST_DELTA_LIST_PAGE is a list page's size: words alone. */
#define HOST_DELTA_LIST_PAGE ((HostDeltaPageSize) {0, (uint32) HOST_DELTA_LIST_FANOUT})

/*
 * HOST_DELTA_LIST_DEEPEST is the most levels a list has, since five hold MOST
 * entries, and the depth and MOST fit their bits.
 */
#define HOST_DELTA_LIST_DEEPEST 5

/* The list's handles name delta's and shared's, which the umbrella gives. */
#include "doltgres/host/delta.h"

/*
 * HostDeltaListList is one of a record's lists, reached through the guard of
 * its record's lock: the record's place, and its shape's table and the
 * list's index in place of their markers.
 */
typedef struct HostDeltaListList
{
	uint64		place;
	const HostSharedShapeOps *s;
	size_t		i;
} HostDeltaListList;

/* HostDeltaListAtTag is At's variants. */
typedef enum HostDeltaListAtTag
{
	HOST_DELTA_LIST_AT_INLINE,
	HOST_DELTA_LIST_AT_PAGED,
} HostDeltaListAtTag;

/* HostDeltaListEntry is one of a list's entries: a word of its record or a word of one of its pages. */
typedef struct HostDeltaListEntry
{
	HostDeltaListAtTag tag;
	union
	{
		/*
		 * inline_ is a word of the record at a place, by its index among the
		 * record's atomics.
		 */
		struct
		{
			uint64		f0;
			size_t		f1;
		}			inline_;
		HostDeltaWord paged;
	}			at;
} HostDeltaListEntry;

/* HostDeltaListListView is a list read under its record's guard. */
typedef struct HostDeltaListListView
{
	HostDeltaListList list;
} HostDeltaListListView;

/*
 * HostDeltaListCursor is one reader's run of reads of a list under one hold
 * of its record's guard, in which no store reaches the list: the leaf page it
 * reached last, which stays so while the guard is held, since the list's
 * entries change only under the guard held exclusively. Zero-initialized, it
 * has read nothing.
 */
typedef struct HostDeltaListCursor
{
	bool		has_leaf;
	uint64		leaf;
	HostDeltaPageRef page;
} HostDeltaListCursor;

/*
 * HostDeltaListListMut is a list read and stored under its record's guard
 * held exclusively, which it keeps for its links, with the lock's index in
 * place of its marker.
 */
typedef struct HostDeltaListListMut
{
	HostDeltaListList list;
	HostSharedGuard *guard;
	size_t		l;
} HostDeltaListListMut;

/* HostDeltaListClosed is a closed list, which no store changes, so its pages are read with no lock. */
typedef struct HostDeltaListClosed
{
	HostDeltaListList list;
} HostDeltaListClosed;

/* HostDeltaListPages walks a closed list's pages: its root, then each level's pages in order, down to its leaves. */
typedef struct HostDeltaListPages
{
	const HostDeltaHold *hold;
	uint64		root;
	uint64		depth;
	/* entries is the list's count of entries past the inline ones. */
	uint64		entries;

	/*
	 * level is the level of the next page to name, zero when every page is
	 * named.
	 */
	uint64		level;
	/* next is the next page's index among its level's pages. */
	uint64		next;
} HostDeltaListPages;

extern const HostDeltaNamesOps HOST_DELTA_LIST_ENTRY_NAMES_OPS;
extern const StdIteratorOps HOST_DELTA_LIST_PAGES_ITERATOR_OPS;

extern size_t host_delta_list_wants(uint64 length, uint64 depth);
extern size_t host_delta_list_pages(uint64 length);
extern HostDeltaListListView host_delta_list_Guard_list(const HostSharedGuard *self,
														const HostSharedShapeOps *s, size_t i);
extern bool host_delta_list_Guard_closed(const HostSharedGuard *self, const HostSharedShapeOps *s,
										 size_t i, HostDeltaListClosed *result);
extern bool host_delta_list_Made_fill_list(const HostDeltaMade *self, const HostSharedShapeOps *s,
										   size_t i, const uint64 *entries, size_t entries_len,
										   StdVec *spare, StdVec *result, HostError *error);
extern HostDeltaListListMut host_delta_list_GuardExclusive_list_mut(HostSharedGuard *self,
																	const HostSharedShapeOps *s, size_t l,
																	size_t i);
extern uint64 host_delta_list_Entry_load(const HostDeltaListEntry *self);
extern uint64 host_delta_list_ListView_len(const HostDeltaListListView *self);
extern bool host_delta_list_ListView_is_empty(const HostDeltaListListView *self);
extern size_t host_delta_list_ListView_wants(const HostDeltaListListView *self);
extern size_t host_delta_list_ListView_reserving(const HostDeltaListListView *self, uint64 entries);
extern bool host_delta_list_ListView_entry(const HostDeltaListListView *self,
										   const HostDeltaHold *hold, uint64 index,
										   HostDeltaListEntry *result, HostError *error);
extern bool host_delta_list_ListView_get(const HostDeltaListListView *self,
										 const HostDeltaHold *hold, uint64 index, uint64 *result,
										 HostError *error);
extern bool host_delta_list_ListView_get_at(const HostDeltaListListView *self, const HostDeltaHold *hold,
											HostDeltaListCursor *cursor, uint64 index, uint64 *result,
											HostError *error);
extern HostDeltaListListView host_delta_list_ListMut_view(const HostDeltaListListMut *self);
extern bool host_delta_list_ListMut_set(HostDeltaListListMut *self, const HostDeltaHold *hold,
										uint64 index, uint64 value, HostError *error);
extern bool host_delta_list_ListMut_set_page(HostDeltaListListMut *self, const HostDeltaHold *hold,
											 uint64 index, HostDeltaOwned *page,
											 HostDeltaPagePage *result, HostError *error);
extern bool host_delta_list_ListMut_push(HostDeltaListListMut *self, const HostDeltaHold *hold,
										 uint64 value, StdVec *spare, HostError *error);
extern bool host_delta_list_ListMut_push_page(HostDeltaListListMut *self,
											  const HostDeltaHold *hold, HostDeltaOwned *page,
											  StdVec *spare, HostDeltaPagePage *result,
											  HostError *error);
extern bool host_delta_list_ListMut_reserve(HostDeltaListListMut *self, const HostDeltaHold *hold,
											uint64 entries, StdVec *spare, HostError *error);
extern bool host_delta_list_ListMut_insert(HostDeltaListListMut *self, const HostDeltaHold *hold,
										   uint64 index, uint64 value, StdVec *spare,
										   HostError *error);
extern bool host_delta_list_ListMut_replace_with(HostDeltaListListMut *self,
												 const HostDeltaHold *hold, uint64 index,
												 const uint64 *values, StdVec *spare,
												 HostError *error);
extern bool host_delta_list_ListMut_remove(HostDeltaListListMut *self, const HostDeltaHold *hold,
										   uint64 index, HostError *error);
extern HostDeltaListClosed host_delta_list_ListMut_close(HostDeltaListListMut *self);
extern HostDeltaListPages host_delta_list_Closed_pages(const HostDeltaListClosed *self,
													   const HostDeltaHold *hold);
extern bool host_delta_list_Pages_Iterator_next(void *self, void *result);

#endif							/* DOLTGRES_HOST_DELTA_LIST_H */
