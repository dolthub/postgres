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
 * The delta's pages (design item 14's S6): the memory of the delta layer as
 * pages of bytes written once and of words, which every backend shares. A
 * page is its allocator's own until a link names it, is read and appended to
 * under a hold, and is freed by a pass once every hold that began before its
 * retirement has ended. No door reads or writes memory a page doesn't own,
 * whatever the names a crate hands it.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "delta.rs"

/* host_delta_hold begins a hold for the running entry. */
bool
host_delta_hold(HostIoIo *io, HostDeltaHold *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	uint64		epoch;

	if (!host.ops->delta_hold(host.self, &raise, &epoch, error))
		return false;
	*result = (HostDeltaHold)
	{
		host, epoch
	};
	return true;
}

/*
 * host_delta_Hold_page opens the page a name names, for as long as the hold
 * lasts, refusing a name that names no page the hold reaches: a number past
 * the directory, another generation, a page neither live nor retired, or one
 * retired before the hold began.
 */
bool
host_delta_Hold_page(const HostDeltaHold *self, HostDeltaPagePage name, HostDeltaPageRef *result,
					 HostError *error)
{
	HostHost	host = self->host;
	HostDeltaDirectoryOpened opened;

	if (!self->host.ops->delta_open(self->host.self, name, self->epoch, &opened, error))
		return false;
	*result = (HostDeltaPageRef)
	{
		host, opened
	};
	return true;
}

/*
 * host_delta_Hold_record opens the record of an aged shape a location names,
 * for as long as the hold lasts, refusing a location that names no record the
 * hold reaches, as page refuses a name.
 */
bool
host_delta_Hold_record(const HostDeltaHold *self, const HostSharedAgedOps *s, uint64 location,
					   HostSharedRecord *result, HostError *error)
{
	HostSharedKind kind = host_shared_Kind_aged(s);
	uint64		place;

	if (!self->host.ops->delta_record(self->host.self, &kind, location, self->epoch, &place, error))
		return false;
	*result = host_shared_Record_at(place);
	return true;
}

/* host_delta_Hold_drop ends the hold. */
void
host_delta_Hold_drop(HostDeltaHold *self)
{
	self->host.ops->delta_release(self->host.self, self->epoch);
}

/* host_delta_owner returns the lock a guard holds, as a page's slot records the lock that may append to it. */
static HostDeltaDirectoryOwner
host_delta_owner(HostHost host, const HostSharedShapeOps *s, size_t l, const HostSharedGuard *guard)
{
	uint64		place = host_shared_Guard_place(guard);
	uint64		location = host.ops->shared_location(host.self, place);

	return (HostDeltaDirectoryOwner)
	{
		s->id, location, (uint64) l
	};
}

/*
 * host_delta_PageRef_bytes lends a range of the page's bytes below its filled
 * mark, loaded at the call, for as long as the hold lasts.
 */
bool
host_delta_PageRef_bytes(const HostDeltaPageRef *self, uint64 offset, uint64 len,
						 HostDeltaLent *result, HostError *error)
{
	HostCacheBytes bytes;

	if (!self->host.ops->delta_bytes(self->host.self, &self->opened, offset, len, &bytes, error))
		return false;
	*result = (HostDeltaLent)
	{
		bytes
	};
	return true;
}

/*
 * host_delta_PageRef_room returns how many bytes the page takes above its
 * filled mark, loaded at the call, and none for a sealed page.
 */
uint64
host_delta_PageRef_room(const HostDeltaPageRef *self)
{
	return self->host.ops->delta_room(self->host.self, &self->opened);
}

/* host_delta_PageRef_filled returns how many bytes the page holds below its filled mark, loaded at the call. */
uint64
host_delta_PageRef_filled(const HostDeltaPageRef *self)
{
	return self->host.ops->delta_filled(self->host.self, &self->opened);
}

/* host_delta_PageRef_words returns the page's count of words. */
uint64
host_delta_PageRef_words(const HostDeltaPageRef *self)
{
	return (uint64) self->opened.size.words;
}

/* host_delta_PageRef_word returns one of the page's words, refusing an index past its count. */
bool
host_delta_PageRef_word(const HostDeltaPageRef *self, uint64 index, HostDeltaWord *result,
						HostError *error)
{
	if (!host_delta_directory_worded(self->opened.size, index, error))
		return false;
	*result = (HostDeltaWord)
	{
		self->host, self->opened.body, index
	};
	return true;
}

/*
 * host_delta_PageRef_append copies a record above the page's filled mark and
 * then advances the mark, returning the record's offset. Its guard is the
 * exclusive guard of the lock the page's slot records, so another record's
 * guard and a sealed page are refused, as is a record that doesn't fit, which
 * changes nothing.
 */
bool
host_delta_PageRef_append(const HostDeltaPageRef *self, const HostSharedShapeOps *s, size_t l,
						  HostSharedGuard *guard, const uint8 *record, size_t record_len,
						  uint32 *result, HostError *error)
{
	HostDeltaDirectoryOwner owner = host_delta_owner(self->host, s, l, guard);

	return self->host.ops->delta_append(self->host.self, &self->opened, owner, record, record_len,
										result, error);
}

/*
 * host_delta_PageRef_seal clears the page's owner under the guard of the lock
 * its slot records, after which the page takes no append; a seal of a page
 * already sealed does nothing, since a split that fails after its seal leaves
 * its range's own pages sealed for the next split of the range to seal again.
 */
bool
host_delta_PageRef_seal(const HostDeltaPageRef *self, const HostSharedShapeOps *s, size_t l,
						HostSharedGuard *guard, HostError *error)
{
	HostDeltaDirectoryOwner owner = host_delta_owner(self->host, s, l, guard);

	return self->host.ops->delta_seal(self->host.self, &self->opened, owner, error);
}

/* host_delta_Lent_Deref_deref is the lent bytes. */
StdBytes
host_delta_Lent_Deref_deref(const HostDeltaLent *self)
{
	return host_cache_Bytes_Deref_deref(&self->bytes);
}

/* host_delta_Lent_drop is Lent's drop glue, its bytes'. */
void
host_delta_Lent_drop(HostDeltaLent *self)
{
	host_cache_Bytes_drop(&self->bytes);
}

/* host_delta_Word_load reads the word. */
uint64
host_delta_Word_load(const HostDeltaWord *self)
{
	HostDeltaWordOp load = {HOST_DELTA_WORD_OP_LOAD};

	return self->host.ops->delta_word(self->host.self, self->body, self->index, load);
}

/* host_delta_Word_store writes the word. */
void
host_delta_Word_store(const HostDeltaWord *self, uint64 value)
{
	HostDeltaWordOp op = {HOST_DELTA_WORD_OP_STORE};

	op.u.store = value;
	(void) self->host.ops->delta_word(self->host.self, self->body, self->index, op);
}

/* host_delta_Word_compare_exchange writes a value if the word holds the one expected, returning the value it held. */
uint64
host_delta_Word_compare_exchange(const HostDeltaWord *self, uint64 expected, uint64 value)
{
	HostDeltaWordOp op = {HOST_DELTA_WORD_OP_EXCHANGE};

	op.u.exchange.f0 = expected;
	op.u.exchange.f1 = value;
	return self->host.ops->delta_word(self->host.self, self->body, self->index, op);
}

/* host_delta_Word_fetch_add adds to the word, wrapping, and returns the value it held. */
uint64
host_delta_Word_fetch_add(const HostDeltaWord *self, uint64 value)
{
	HostDeltaWordOp op = {HOST_DELTA_WORD_OP_ADD};

	op.u.add = value;
	return self->host.ops->delta_word(self->host.self, self->body, self->index, op);
}

/* host_delta_Word_Takes_take stores a page's name in the word. */
static void
host_delta_Word_Takes_take(void *self, uint64 name)
{
	host_delta_Word_store((const HostDeltaWord *) self, name);
}

/* HOST_DELTA_WORD_NAMES_OPS is Word's table of Names. */
const HostDeltaNamesOps HOST_DELTA_WORD_NAMES_OPS = {{host_delta_Word_Takes_take}};

/* host_delta_Named_Takes_take stores a page's name in the record's atomic. */
static void
host_delta_Named_Takes_take(void *self, uint64 name)
{
	host_shared_Named_store((const HostSharedNamed *) self, name);
}

/* HOST_DELTA_NAMED_NAMES_OPS is Named's table of Names. */
const HostDeltaNamesOps HOST_DELTA_NAMED_NAMES_OPS = {{host_delta_Named_Takes_take}};

/* host_delta_Past_Takes_take stores one past a page's name in the record's atomic. */
static void
host_delta_Past_Takes_take(void *self, uint64 name)
{
	host_shared_Past_store((const HostSharedPast *) self, name);
}

/* HOST_DELTA_PAST_NAMES_OPS is Past's table of Names. */
const HostDeltaNamesOps HOST_DELTA_PAST_NAMES_OPS = {{host_delta_Past_Takes_take}};

/* host_delta_RunWord_Takes_take stores a page's name in the single record's word. */
static void
host_delta_RunWord_Takes_take(void *self, uint64 name)
{
	host_shared_RunWord_store((const HostSharedRunWord *) self, name);
}

/* HOST_DELTA_RUN_WORD_NAMES_OPS is RunWord's table of Names. */
const HostDeltaNamesOps HOST_DELTA_RUN_WORD_NAMES_OPS = {{host_delta_RunWord_Takes_take}};

/* host_delta_Owned_name returns the page's name, which its link will store. */
HostDeltaPagePage
host_delta_Owned_name(const HostDeltaOwned *self)
{
	return self->name;
}

/* host_delta_Owned_append copies a record above the page's filled mark and returns its offset, refusing one that doesn't fit. */
bool
host_delta_Owned_append(HostDeltaOwned *self, const uint8 *record, size_t record_len,
						uint32 *result, HostError *error)
{
	return self->host.ops->delta_owned_append(self->host.self, self->name, record, record_len,
											  result, error);
}

/* host_delta_Owned_store writes one of the page's words, refusing an index past its count. */
bool
host_delta_Owned_store(HostDeltaOwned *self, uint64 index, uint64 value, HostError *error)
{
	return self->host.ops->delta_owned_store(self->host.self, self->name, index, value, error);
}

/*
 * host_delta_Owned_link records the guard's lock as the one that may append
 * to the page, makes the page live, and stores its name in the given word,
 * with nothing that can fail between them; it takes the page, whose drop
 * follows.
 */
HostDeltaPagePage
host_delta_Owned_link(HostDeltaOwned *self, const HostSharedShapeOps *s, size_t l,
					  HostSharedGuard *guard, HostDeltaNames word)
{
	HostDeltaPagePage name;
	volatile bool self_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostDeltaDirectoryOwner owner = host_delta_owner(self->host, s, l, guard);
		uint64		value;

		self->host.ops->delta_link(self->host.self, self->name, owner);
		value = host_delta_page_Page_value(self->name);
		word.ops->takes.take(word.self, value);
		name = self->name;
		self_live = false;
		host_delta_Owned_drop(self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (self_live)
			host_delta_Owned_drop(self);
		std_unwind_resume(&unwind);
	}
	return name;
}

/*
 * host_delta_Owned_drop gives back a page its owner dropped unlinked; the
 * host does nothing for a linked one. It takes the page's address untyped, as
 * a vector's drop of its elements takes it.
 */
void
host_delta_Owned_drop(void *self)
{
	HostDeltaOwned *owned = (HostDeltaOwned *) self;

	owned->host.ops->delta_give_back(owned->host.self, owned->name);
}

/*
 * host_delta_Unlinked_drop gives back a record its maker dropped unlinked,
 * which counts as a free; the host does nothing for a linked one. It takes
 * the record's address untyped, as a vector's drop of its elements takes it.
 */
void
host_delta_Unlinked_drop(void *self)
{
	HostDeltaUnlinked *unlinked = (HostDeltaUnlinked *) self;

	unlinked->host.ops->delta_unmake(unlinked->host.self, unlinked->place);
}

/*
 * host_delta_make makes a record of an aged shape, its parts cleared, which
 * its maker owns until its link. A refusal is the budget's signal, as
 * alloc's is: the maker drops what it owns, waits, and asks again.
 */
bool
host_delta_make(HostIoIo *io, const HostSharedAgedOps *s, bool *result_ok, HostDeltaMade *result_made,
				HostDeltaFull *result_full, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	HostSharedKind kind = host_shared_Kind_aged(s);
	bool		ok;
	uint64		place;
	uint64		location;
	uint64		frees;

	if (!host.ops->delta_make(host.self, &raise, &kind, s->name.ptr, s->name.len, &ok, &place,
							  &location, &frees, error))
		return false;
	*result_ok = ok;
	if (ok)
	{
		*result_made = (HostDeltaMade)
		{
			{
				host, place
			}, location
		};
	}
	else
	{
		*result_full = (HostDeltaFull)
		{
			frees
		};
	}
	return true;
}

/* host_delta_Made_location returns the record's location, which its link will store. */
uint64
host_delta_Made_location(const HostDeltaMade *self)
{
	return self->location;
}

/* host_delta_Made_atomic operates on one of the record's atomics, returning its value before. */
uint64
host_delta_Made_atomic(const HostDeltaMade *self, size_t a, HostSharedOp op)
{
	return host_shared_Record_atomic(host_shared_Record_at(self->unlinked.place), a, op);
}

/* host_delta_Made_names returns one of the record's atomics as a word that takes a name at a link. */
HostSharedNamed
host_delta_Made_names(const HostDeltaMade *self, size_t a)
{
	return host_shared_Record_names(host_shared_Record_at(self->unlinked.place), a);
}

/*
 * host_delta_Made_link makes live every page the given With holds, each
 * owned by the lock it was given with, publishes every record it holds and
 * then this record, and stores this record's location in the given word,
 * returning it. Nothing in it can fail, so no error and no exit falls between
 * its stores. It takes the record and the With, whose drops follow.
 */
uint64
host_delta_Made_link(HostDeltaMade *self, const HostSharedAgedOps *s, HostDeltaNames word,
					 HostDeltaWith *with)
{
	HostHost	host = self->unlinked.host;
	uint64		location;
	volatile bool self_live = true;
	volatile bool with_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		for (size_t i = 0, i_len = with->own.len; i < i_len; i++)
		{
			const host_delta_With_own_t *own = &STD_VEC_AT(&with->own, host_delta_With_own_t, i);
			HostDeltaDirectoryOwner owner = {s->shape.id, self->location, own->f1};

			host.ops->delta_link(host.self, own->f0.name, owner);
		}
		for (size_t i = 0, i_len = with->pages.len; i < i_len; i++)
		{
			const host_delta_With_pages_t *page = &STD_VEC_AT(&with->pages, host_delta_With_pages_t, i);

			host.ops->delta_link(host.self, page->f0.name, page->f1);
		}
		for (size_t i = 0, i_len = with->records.len; i < i_len; i++)
			host.ops->delta_publish(host.self, STD_VEC_AT(&with->records, HostDeltaUnlinked, i).place);
		host.ops->delta_publish(host.self, self->unlinked.place);
		word.ops->takes.take(word.self, self->location);
		location = self->location;
		with_live = false;
		host_delta_With_drop(with);
		self_live = false;
		host_delta_Made_drop(self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (with_live)
			host_delta_With_drop(with);
		if (self_live)
			host_delta_Made_drop(self);
		std_unwind_resume(&unwind);
	}
	return location;
}

/* host_delta_Made_drop is Made's drop glue, its unlinked record's. */
void
host_delta_Made_drop(HostDeltaMade *self)
{
	host_delta_Unlinked_drop(&self->unlinked);
}

/* host_delta_With_Default_default returns a With of nothing. */
HostDeltaWith
host_delta_With_Default_default(void)
{
	HostDeltaWith with;

	std_vec_new(&with.own, sizeof(host_delta_With_own_t), _Alignof(host_delta_With_own_t));
	std_vec_new(&with.pages, sizeof(host_delta_With_pages_t), _Alignof(host_delta_With_pages_t));
	std_vec_new(&with.records, sizeof(HostDeltaUnlinked), _Alignof(HostDeltaUnlinked));
	return with;
}

/* host_delta_With_page adds a page the linked record's lock will own; it takes the page. */
bool
host_delta_With_page(HostDeltaWith *self, size_t l, HostDeltaOwned *page, HostError *error)
{
	volatile bool page_live = true;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_delta_With_own_t own;

		if (!std_vec_try_reserve(&self->own, 1, sizeof(host_delta_With_own_t), _Alignof(host_delta_With_own_t)))
		{
			g_error = host_Error_out_of_memory();
			g_error_live = true;
			page_live = false;
			host_delta_Owned_drop(page);
			std_unwind_pop(&unwind);
			*error = *(HostError *) &g_error;
			return false;
		}
		own.f0 = *page;
		own.f1 = (uint64) l;
		page_live = false;
		std_vec_push_Checked(&self->own, &own, sizeof(host_delta_With_own_t), _Alignof(host_delta_With_own_t));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (page_live)
			host_delta_Owned_drop(page);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * host_delta_With_record adds an unlinked record under the linked one, with
 * the pages its lock will own; it takes the record and the pages' iterator,
 * whose items are HostDeltaOwned.
 */
bool
host_delta_With_record(HostDeltaWith *self, const HostSharedAgedOps *r, size_t l, HostDeltaMade *made,
					   StdIterator pages, HostError *error)
{
	HostDeltaDirectoryOwner owner = {r->shape.id, made->location, (uint64) l};
	volatile bool made_live = true;
	volatile bool pages_live = true;
	volatile HostDeltaOwned page;
	volatile bool page_live = false;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		if (!std_vec_try_reserve(&self->records, 1, sizeof(HostDeltaUnlinked), _Alignof(HostDeltaUnlinked)))
		{
			g_error = host_Error_out_of_memory();
			g_error_live = true;
			pages_live = false;
			std_iterator_drop(&pages);
			made_live = false;
			host_delta_Made_drop(made);
			std_unwind_pop(&unwind);
			*error = *(HostError *) &g_error;
			return false;
		}
		made_live = false;
		std_vec_push_Checked(&self->records, &made->unlinked, sizeof(HostDeltaUnlinked), _Alignof(HostDeltaUnlinked));
		while (pages.ops->next(pages.self, (HostDeltaOwned *) &page))
		{
			host_delta_With_pages_t added;

			page_live = true;
			if (!std_vec_try_reserve(&self->pages, 1, sizeof(host_delta_With_pages_t), _Alignof(host_delta_With_pages_t)))
			{
				g_error = host_Error_out_of_memory();
				g_error_live = true;
				page_live = false;
				host_delta_Owned_drop((HostDeltaOwned *) &page);
				pages_live = false;
				std_iterator_drop(&pages);
				std_unwind_pop(&unwind);
				*error = *(HostError *) &g_error;
				return false;
			}
			added.f0 = *(HostDeltaOwned *) &page;
			added.f1 = owner;
			page_live = false;
			std_vec_push_Checked(&self->pages, &added, sizeof(host_delta_With_pages_t), _Alignof(host_delta_With_pages_t));
		}
		pages_live = false;
		std_iterator_drop(&pages);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (page_live)
			host_delta_Owned_drop((HostDeltaOwned *) &page);
		if (pages_live)
			std_iterator_drop(&pages);
		if (made_live)
			host_delta_Made_drop(made);
		std_unwind_resume(&unwind);
	}
	return true;
}

/* host_delta_TupleOwnedU64_drop is the drop glue of an element of With's own, its page's. */
static void
host_delta_TupleOwnedU64_drop(void *value)
{
	host_delta_Owned_drop(&((host_delta_With_own_t *) value)->f0);
}

/* host_delta_TupleOwnedOwner_drop is the drop glue of an element of With's pages, its page's. */
static void
host_delta_TupleOwnedOwner_drop(void *value)
{
	host_delta_Owned_drop(&((host_delta_With_pages_t *) value)->f0);
}

/*
 * host_delta_With_drop is With's drop glue: its own's, its pages', and its
 * records', each still dropped should an earlier one's drop panic.
 */
void
host_delta_With_drop(HostDeltaWith *self)
{
	volatile bool pages_live = true;
	volatile bool records_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_drop_Checked(&self->own, sizeof(host_delta_With_own_t), _Alignof(host_delta_With_own_t), host_delta_TupleOwnedU64_drop);
		pages_live = false;
		std_vec_drop_Checked(&self->pages, sizeof(host_delta_With_pages_t), _Alignof(host_delta_With_pages_t), host_delta_TupleOwnedOwner_drop);
		records_live = false;
		std_vec_drop_Checked(&self->records, sizeof(HostDeltaUnlinked), _Alignof(HostDeltaUnlinked), host_delta_Unlinked_drop);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pages_live)
			std_vec_drop_Checked(&self->pages, sizeof(host_delta_With_pages_t), _Alignof(host_delta_With_pages_t), host_delta_TupleOwnedOwner_drop);
		if (records_live)
			std_vec_drop_Checked(&self->records, sizeof(HostDeltaUnlinked), _Alignof(HostDeltaUnlinked), host_delta_Unlinked_drop);
		std_unwind_resume(&unwind);
	}
}

/* host_delta_Leaving_of returns the record of an aged shape at a location. */
HostDeltaLeaving
host_delta_Leaving_of(const HostSharedAgedOps *s, uint64 location)
{
	return (HostDeltaLeaving)
	{
		s->shape.id, location
	};
}

/*
 * host_delta_alloc takes every page a write may need, all of them or none. A
 * refusal is the budget's signal, never a user's error: the writer drops what
 * it owns, waits, and asks again. Its one error of a size is one an empty
 * delta couldn't give. Its result is a Vec of HostDeltaOwned when result_ok,
 * and the refusal's Full otherwise.
 */
bool
host_delta_alloc(HostIoIo *io, const HostDeltaPageSize *sizes, size_t sizes_len, bool *result_ok,
				 StdVec *result_owned, HostDeltaFull *result_full, HostError *error)
{
	HostHost	host;
	volatile StdVec names;
	volatile bool names_live = false;
	volatile StdVec owned;
	volatile bool owned_live = false;
	volatile StdVecIntoIter iter;
	volatile bool iter_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostMayRaise raise;
		bool		has_frees;
		uint64		frees;
		StdVecIntoIter *it = (StdVecIntoIter *) &iter;
		StdVec	   *into = (StdVec *) &owned;

		host = host_current();
		std_vec_new((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		names_live = true;
		std_vec_new((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
		owned_live = true;
		if (!std_vec_try_reserve_exact((StdVec *) &names, sizes_len, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage)))
		{
			*error = host_Error_out_of_memory();
			owned_live = false;
			std_vec_drop_Checked((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		if (!std_vec_try_reserve_exact((StdVec *) &owned, sizes_len, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned)))
		{
			*error = host_Error_out_of_memory();
			owned_live = false;
			std_vec_drop_Checked((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		if (!host.ops->delta_alloc(host.self, &raise, sizes, sizes_len, (StdVec *) &names, &has_frees,
								   &frees, error))
		{
			owned_live = false;
			std_vec_drop_Checked((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		if (has_frees)
		{
			*result_ok = false;
			*result_full = (HostDeltaFull)
			{
				frees
			};
			owned_live = false;
			std_vec_drop_Checked((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return true;
		}
		names_live = false;
		*it = std_Vec_IntoIterator_into_iter((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage),
											 NULL);
		iter_live = true;
		std_vec_extend_trusted_reserve(into, std_VecIntoIter_Iterator_size_hint(it), sizeof(HostDeltaOwned),
									   _Alignof(HostDeltaOwned));
		while (it->next != it->buf.len)
		{
			HostDeltaOwned page = {host, STD_VEC_AT(&it->buf, HostDeltaPagePage, it->next)};

			it->next++;
			STD_VEC_AT(into, HostDeltaOwned, into->len) = page;
			into->len++;
		}
		iter_live = false;
		std_vec_free_Checked(&it->buf, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (iter_live)
			std_VecIntoIter_drop((StdVecIntoIter *) &iter);
		if (owned_live)
			std_vec_drop_Checked((StdVec *) &owned, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
		if (names_live)
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		std_unwind_resume(&unwind);
	}
	*result_ok = true;
	*result_owned = *(StdVec *) &owned;
	return true;
}

/*
 * host_delta_alloc_replacing takes a page of a size for the copy of a page it
 * replaces, a compaction's, which the budget lets one replacement at a time
 * take past it (bugs row 110); it answers as host_delta_alloc does, its result
 * a HostDeltaOwned when result_ok.
 */
bool
host_delta_alloc_replacing(HostIoIo *io, HostDeltaPageSize size, HostDeltaPagePage replaced, bool *result_ok,
						   HostDeltaOwned *result_owned, HostDeltaFull *result_full, HostError *error)
{
	HostHost	host;
	volatile StdVec names;
	volatile bool names_live = false;
	HostDeltaPagePage name;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostMayRaise raise;
		bool		has_frees;
		uint64		frees;

		host = host_current();
		std_vec_new((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		names_live = true;
		if (!std_vec_try_reserve_exact((StdVec *) &names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage)))
		{
			*error = host_Error_out_of_memory();
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		if (!host.ops->delta_alloc_replacing(host.self, &raise, size, replaced, (StdVec *) &names, &has_frees,
											 &frees, error))
		{
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		if (has_frees)
		{
			*result_ok = false;
			*result_full = (HostDeltaFull)
			{
				frees
			};
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return true;
		}
		if (!std_vec_pop((StdVec *) &names, &name, sizeof(HostDeltaPagePage)))
		{
			static const char text[] = "a replacement took no page";
			StdString	message;

			std_string_from_Checked(&message, text, sizeof(text) - 1);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
			std_unwind_pop(&unwind);
			return false;
		}
		names_live = false;
		std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (names_live)
			std_vec_free_Checked((StdVec *) &names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		std_unwind_resume(&unwind);
	}
	*result_ok = true;
	result_owned->host = host;
	result_owned->name = name;
	return true;
}

/*
 * host_delta_wait waits at the budget for a free made since a refusal,
 * returning at once when one was made already. It refuses a process that has
 * a hold, owns a page, or owns an unlinked record, so a waiter keeps nothing
 * from being freed; a cancel ends it with Postgres' error. It takes the
 * refusal.
 */
bool
host_delta_wait(HostIoIo *io, HostDeltaFull *full, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->delta_wait(host.self, &raise, full->frees, error);
}

/* HOST_DELTA_BATCH is the most names one take of the epoch lock retires. */
#define HOST_DELTA_BATCH 64

/*
 * host_delta_retire_closure_0 is retire's last: the final batch's
 * retirement, when it holds anything. Its captures are retire's host.
 */
static void
host_delta_retire_closure_0(void *captures, const HostDeltaLeaving *records, size_t records_len,
							const HostDeltaPagePage *names, size_t names_len)
{
	HostHost   *host = (HostHost *) captures;

	if (!(records_len == 0) || !(names_len == 0))
		host->ops->delta_retire(host->self, records, records_len, names, names_len);
}

/*
 * host_delta_retire retires records and the pages an iterator names, a batch
 * at a time: each batch gets one stamp and one advance of the epoch under one
 * take of the epoch lock, the records going with the first, a record that
 * isn't published or a name that isn't live doing nothing, and the free pass
 * runs once after the last. Its own steps can't fail and check for no
 * interrupt. An error the iterator yields ends the draining: the records and
 * every name yielded before it are retired, and the error is returned. Its
 * caller shows the entry's Io, of which it reads nothing, so the iterator's
 * code reaches no door that waits or takes a lock. It takes the iterator,
 * whose items are HostDeltaRetireItem.
 */
bool
host_delta_retire(HostIoIo *io, const HostDeltaLeaving *records, size_t records_len,
				  StdIterator pages, HostError *error)
{
	HostHost	host;
	HostDeltaPagePage batch[HOST_DELTA_BATCH];
	const HostDeltaLeaving *leaving;
	size_t		leaving_len;
	bool		yielded;
	volatile bool pages_live = true;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostDeltaPagePage named;

		host = host_current();
		named = host_delta_page_Page_named(0);
		for (int i = 0; i < HOST_DELTA_BATCH; i++)
			batch[i] = named;
		leaving = records;
		leaving_len = records_len;
		for (;;)
		{
			size_t		len = 0;
			bool		done = false;

			while (len < HOST_DELTA_BATCH)
			{
				HostDeltaRetireItem item;

				if (pages.ops->next(pages.self, &item))
				{
					if (item.is_ok)
					{
						std_index_check(len, HOST_DELTA_BATCH, RUST_FILE, 479, 21);
						batch[len] = item.ok;
						len = STD_ADD(usize, len, 1, 480, 21);
					}
					else
					{
						g_error = item.err;
						g_error_live = true;
						std_range_to_check(len, HOST_DELTA_BATCH, RUST_FILE, 483, 41);
						host_delta_retire_closure_0(&host, leaving, leaving_len, batch, len);
						yielded = false;
						done = true;
						break;
					}
				}
				else
				{
					std_range_to_check(len, HOST_DELTA_BATCH, RUST_FILE, 487, 41);
					host_delta_retire_closure_0(&host, leaving, leaving_len, batch, len);
					yielded = true;
					done = true;
					break;
				}
			}
			if (done)
				break;
			host.ops->delta_retire(host.self, leaving, leaving_len, batch, HOST_DELTA_BATCH);
			leaving = STD_DANGLING;
			leaving_len = 0;
		}
		host.ops->delta_pass(host.self);
		pages_live = false;
		std_iterator_drop(&pages);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (pages_live)
			std_iterator_drop(&pages);
		std_unwind_resume(&unwind);
	}
	if (!yielded)
		*error = *(HostError *) &g_error;
	return yielded;
}

/*
 * host_delta_advanced advances the epoch and returns the epoch it passed,
 * which every reader and hold begun before the call started at or below and
 * every one begun after it started above, and the oldest one a live reader
 * or hold started at, so a removal deferred at the passed epoch knows when
 * every reader that may still read what it removes ended (bugs row 124).
 */
void
host_delta_advanced(uint64 *result_0, uint64 *result_1)
{
	HostHost	host = host_current();

	host.ops->delta_advanced(host.self, result_0, result_1);
}

/* host_delta_used returns the bytes of the delta in use: its owned, live, and retired pages, and a slot left freeing. */
uint64
host_delta_used(void)
{
	HostHost	host = host_current();

	return host.ops->delta_used(host.self);
}

/* host_delta_budget returns the delta's memory in bytes, delta_memory. */
uint64
host_delta_budget(void)
{
	HostHost	host = host_current();

	return host.ops->delta_budget(host.self);
}

/*
 * host_delta_seed returns the server's random seed of the delta's shapes in
 * memory, such as a skip list's heights, drawn anew at each start of its
 * shared memory; it shapes nothing stored or returned, so a seed that differs
 * by run or platform changes no result.
 */
void
host_delta_seed(uint8 *result)
{
	HostHost	host = host_current();

	host.ops->delta_seed(host.self, result);
}
