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
 * Shared memory (design item 14's S2): records a storage crate declares by a
 * shape of typed parts, reached only through host's doors by part, so no
 * storage crate sees an address. All of it is fixed memory, sized when the
 * server starts: a kind is a pool of a shape's records, a single record, or
 * words for each process, listed once in the server's table of kinds. A
 * record's locks are taken only in the order its shape declares, a blob and
 * an array's entry are written only under their own lock held exclusively, a
 * single record's slots are claimed only under the barrier of the pooled
 * record they name, and a lock held across I/O lends a capability that never
 * waits, all by the parts' types in the Rust, which the C's callers keep: a
 * record's locks and flags, and the epochs, are taken from the backend's Io
 * itself, which every guard has borrowed. A pooled record is reached only
 * through a handle that keeps its slot: its owner's, who is its maker until
 * it publishes it and then the one process that claims it, a holder's, or a
 * reclaim's. A shape's, lock's, flag's, atomic's, and word's marker type is
 * its table or its index here.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta/list.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "shared.rs"

/* The items below that an earlier item reaches, declared ahead so the file keeps shared.rs's order. */
static HostError host_shared_unnamed(void);
static bool host_shared_take(HostHost host, StdAny raise, uint64 place, size_t lock, bool exclusive,
							 HostError *error);
static HostSharedGuard host_shared_Guard_new(uint64 place, size_t index, HostIoHeld *held, StdAny raise);
static HostSharedLongGuard host_shared_LongGuard_new(uint64 place, size_t index, HostIoHeld *held,
													 StdAny raise);

/* host_shared_Single_words is Single's words for a shape that declares no run: none. */
size_t
host_shared_Single_words(const HostSharedLimits *limits)
{
	return 0;
}

/* host_shared_Kind_of returns a shape's line in a form. */
static HostSharedKind
host_shared_Kind_of(const HostSharedShapeOps *s, HostSharedForm form)
{
	size_t		tmp_1;
	size_t		atomics;

	if (!(s->blobs_len <= HOST_SHARED_BLOBS))
	{
		static const char text[] = "a shape declares more blobs than a record holds";

		std_panic(RUST_FILE, 199, 9, text, sizeof(text) - 1);
	}
	if (!(s->arrays_len <= HOST_SHARED_ARRAYS))
	{
		static const char text[] = "a shape declares more arrays than a record holds";

		std_panic(RUST_FILE, 200, 9, text, sizeof(text) - 1);
	}
	tmp_1 = STD_MUL(usize, HOST_DELTA_LIST_WORDS, s->lists, 205, 35);
	atomics = STD_ADD(usize, s->atomics, tmp_1, 205, 22);
	return (HostSharedKind)
	{
		.id = s->id,
			.locks = s->locks,
			.flags = s->flags,
			.atomics = atomics,
			.blobs = s->blobs,
			.blobs_len = s->blobs_len,
			.arrays = s->arrays,
			.arrays_len = s->arrays_len,
			.form = form,
	};
}

/* host_shared_Kind_pool returns the line of a pool. */
HostSharedKind
host_shared_Kind_pool(const HostSharedPoolOps *s)
{
	HostSharedForm form = {HOST_SHARED_FORM_POOL, {.pool = s->count}};

	return host_shared_Kind_of(&s->shape, form);
}

/*
 * host_shared_Kind_aged returns the line of a pool the delta's epochs free,
 * whose records hold two atomics past their shape's, the host's: the epoch of
 * their retirement, and their link in their kind's retired list (bugs row
 * 309).
 */
HostSharedKind
host_shared_Kind_aged(const HostSharedAgedOps *s)
{
	HostSharedForm form = {HOST_SHARED_FORM_AGED};
	HostSharedKind kind;

	form.u.aged.count = s->count;
	form.u.aged.names = s->names;
	form.u.aged.names_len = s->names_len;
	kind = host_shared_Kind_of(&s->shape, form);
	kind.atomics = STD_ADD(usize, kind.atomics, 2, 221, 9);
	return kind;
}

/* host_shared_Kind_single returns the line of a single record. */
HostSharedKind
host_shared_Kind_single(const HostSharedSingleOps *s)
{
	HostSharedForm form = {HOST_SHARED_FORM_SINGLE};

	form.u.single.run = s->words;
	return host_shared_Kind_of(&s->shape, form);
}

/* host_shared_Kind_slots returns the line of a slot for each process. */
HostSharedKind
host_shared_Kind_slots(const HostSharedProcSlotsOps *p)
{
	HostSharedForm form = {HOST_SHARED_FORM_PROCESS};

	form.u.process.kept = p->kept;
	return (HostSharedKind)
	{
		.id = p->id,
			.atomics = p->words,
			.blobs = STD_DANGLING,
			.arrays = STD_DANGLING,
			.form = form,
	};
}

/*
 * host_shared_ordered reports whether a table lists every aged kind before
 * each aged kind it names and names only aged kinds the table lists, so a
 * free pass that walks the kinds in the table's order frees a record no later
 * than what it names.
 */
bool
host_shared_ordered(const HostSharedKind *kinds, size_t kinds_len)
{
	size_t		at = 0;

	while (at < kinds_len)
	{
		std_index_check(at, kinds_len, RUST_FILE, 250, 44);
		if (kinds[at].form.tag == HOST_SHARED_FORM_AGED)
		{
			const uint64 *names = kinds[at].form.u.aged.names;
			size_t		names_len = kinds[at].form.u.aged.names_len;
			size_t		named = 0;

			while (named < names_len)
			{
				size_t		other = 0;

				while (other < kinds_len &&
					   (std_index_check(other, kinds_len, RUST_FILE, 254, 46),
						std_index_check(named, names_len, RUST_FILE, 254, 65),
						kinds[other].id != names[named]))
					other = STD_ADD(usize, other, 1, 255, 21);
				if (other == kinds_len || other <= at ||
					(std_index_check(other, kinds_len, RUST_FILE, 257, 69),
					 kinds[other].form.tag != HOST_SHARED_FORM_AGED))
					return false;
				named = STD_ADD(usize, named, 1, 260, 17);
			}
		}
		at = STD_ADD(usize, at, 1, 263, 9);
	}
	return true;
}

/*
 * host_shared_array returns the line of a shape's ARRAYS for its array at an
 * index, with a count of entries and the size of the entry the array's
 * declaration gives.
 */
void
host_shared_array(size_t entry_size, size_t count, size_t *out_0, size_t *out_1)
{
	*out_0 = count;
	*out_1 = entry_size;
}

/*
 * host_shared_distinct reports whether indices are different from one another
 * and below a count, which a shape's crate asserts of its markers at compile
 * time.
 */
bool
host_shared_distinct(const size_t *indices, size_t indices_len, size_t count)
{
	size_t		i = 0;

	while (i < indices_len)
	{
		size_t		j;

		std_index_check(i, indices_len, RUST_FILE, 385, 12);
		if (indices[i] >= count)
			return false;
		j = STD_ADD(usize, i, 1, 388, 21);
		while (j < indices_len)
		{
			std_index_check(i, indices_len, RUST_FILE, 390, 16);
			std_index_check(j, indices_len, RUST_FILE, 390, 30);
			if (indices[i] == indices[j])
				return false;
			j = STD_ADD(usize, j, 1, 393, 13);
		}
		i = STD_ADD(usize, i, 1, 395, 9);
	}
	return true;
}

/*
 * host_shared_make makes a record of a pool, its atomics zero, its blobs
 * empty, and its flags clear, owned by its maker. When every slot is taken it
 * has the pool reclaim what it can and looks again, for as long as the
 * reclaim says to, and fails only once every record of the pool is in use.
 */
bool
host_shared_make(const HostSharedPoolOps *s, HostIoIo *io, HostSharedOwned *result, HostError *error)
{
	HostHost	host = host_current();

	for (;;)
	{
		HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		HostSharedKind kind = host_shared_Kind_pool(s);
		bool		has_place;
		uint64		place;
		bool		again;

		if (!host.ops->shared_make(host.self, &raise, &kind, &has_place, &place, error))
			return false;
		if (has_place)
		{
			*result = (HostSharedOwned)
			{
				host, place
			};
			return true;
		}
		if (!s->reclaim(io, &again, error))
			return false;
		if (!again)
		{
			raise = host_doors(HOST_IO_IO_READ_DOORS(io));
			kind = host_shared_Kind_pool(s);
			if (!host.ops->shared_make(host.self, &raise, &kind, &has_place, &place, error))
				return false;
			if (has_place)
			{
				*result = (HostSharedOwned)
				{
					host, place
				};
				return true;
			}
			else
			{
				static const StdFmtPiece pieces[] = {
					STD_FMT_LIT("every "), STD_FMT_ARG(0), STD_FMT_LIT(" record is in use"),
				};
				StdFmtArg	args[] = {{&s->name, std_str_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*error = host_Error_fork((const uint8 *) "XX000", &message);
				return false;
			}
		}
		raise = host_doors(HOST_IO_IO_READ_DOORS(io));
		if (!host.ops->check_interrupts(host.self, &raise, error))
			return false;
	}
}

/*
 * host_shared_claim takes the ownership of a published record at a
 * location, whatever processes hold it, after which no process opens it. It
 * answers nothing for a location that is stale or a record that isn't
 * published. Its caller shows the entry's Io or the guard it read the
 * location under, of which the claim reads nothing.
 */
bool
host_shared_claim(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
				  bool *has_result, HostSharedOwned *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_pool(s);
	bool		has_place;
	uint64		place;

	if (!host.ops->shared_claim(host.self, &kind, location, &has_place, &place, error))
		return false;
	*has_result = has_place;
	if (has_place)
		*result = (HostSharedOwned)
	{
		host, place
	};
	return true;
}

/*
 * host_shared_free frees a record: at once when no process holds it, and
 * otherwise when the last that does closes it; it takes the handle, whose
 * drop follows.
 */
void
host_shared_free(HostSharedOwned *owned)
{
	volatile bool owned_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		owned->host.ops->shared_free(owned->host.self, owned->place);
		owned_live = false;
		host_shared_Owned_drop(owned);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (owned_live)
			host_shared_Owned_drop(owned);
		std_unwind_resume(&unwind);
	}
}

/* host_shared_Owned_record returns the record, reached for as long as its owner is. */
HostSharedRecord
host_shared_Owned_record(const HostSharedOwned *self)
{
	return (HostSharedRecord)
	{
		self->place
	};
}

/*
 * host_shared_Owned_publish gives up the ownership, so any process may open
 * the record and one may claim it, and returns its location for the owner to
 * publish; it takes the handle, whose drop follows.
 */
uint64
host_shared_Owned_publish(HostSharedOwned *self)
{
	uint64		location;
	volatile bool self_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		location = self->host.ops->shared_publish(self->host.self, self->place);
		self_live = false;
		host_shared_Owned_drop(self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (self_live)
			host_shared_Owned_drop(self);
		std_unwind_resume(&unwind);
	}
	return location;
}

/* host_shared_Owned_drop gives the record up: one never published is freed, and one claimed is published again. */
void
host_shared_Owned_drop(HostSharedOwned *self)
{
	self->host.ops->shared_give_up(self->host.self, self->place);
}

/* host_shared_Io_Raises_raise returns the capability of pg's an Io borrows. */
StdAny
host_shared_Io_Raises_raise(void *self)
{
	HostIoHeld *held;
	StdAny		raise;

	host_io_Io_Sealed_parts(self, &held, &raise);
	return raise;
}

/* host_shared_Registering_Raises_raise returns the capability of pg's a registration's view borrows. */
StdAny
host_shared_Registering_Raises_raise(void *self)
{
	HostIoHeld *held;
	StdAny		raise;

	host_io_Registering_Sealed_parts(self, &held, &raise);
	return raise;
}

/* host_shared_Guard_Raises_raise returns the capability of pg's a guard borrows. */
StdAny
host_shared_Guard_Raises_raise(void *self)
{
	return ((HostSharedGuard *) self)->raise;
}

/* host_shared_LongGuard_Raises_raise returns the capability of pg's a long guard borrows. */
StdAny
host_shared_LongGuard_Raises_raise(void *self)
{
	return ((HostSharedLongGuard *) self)->raise;
}

/* host_shared_KeyIo_Raises_raise returns the capability of pg's a key lock's view borrows. */
StdAny
host_shared_KeyIo_Raises_raise(void *self)
{
	HostIoHeld *held;
	StdAny		raise;

	host_io_KeyIo_Sealed_parts(self, &held, &raise);
	return raise;
}

/* host_shared_LongIo_Raises_raise returns the capability of pg's a long lock's capability borrows. */
StdAny
host_shared_LongIo_Raises_raise(void *self)
{
	return ((HostSharedLongIo *) self)->raise;
}

/* host_shared_FlagIo_Raises_raise returns the capability of pg's a flag's capability borrows. */
StdAny
host_shared_FlagIo_Raises_raise(void *self)
{
	return ((HostSharedFlagIo *) self)->raise;
}

/* HOST_SHARED_IO_WITHIN_OPS is Io's table of Within. */
const HostSharedWithinOps HOST_SHARED_IO_WITHIN_OPS = {{host_shared_Io_Raises_raise}};

/* HOST_SHARED_GUARD_WITHIN_OPS is Guard's table of Within. */
const HostSharedWithinOps HOST_SHARED_GUARD_WITHIN_OPS = {{host_shared_Guard_Raises_raise}};

/* HOST_SHARED_LONG_GUARD_WITHIN_OPS is LongGuard's table of Within. */
const HostSharedWithinOps HOST_SHARED_LONG_GUARD_WITHIN_OPS = {{host_shared_LongGuard_Raises_raise}};

/* HOST_SHARED_IO_PAUSES_OPS is Io's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_IO_PAUSES_OPS = {{host_shared_Io_Raises_raise}};

/* HOST_SHARED_REGISTERING_PAUSES_OPS is Registering's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_REGISTERING_PAUSES_OPS = {{host_shared_Registering_Raises_raise}};

/* HOST_SHARED_KEY_IO_PAUSES_OPS is KeyIo's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_KEY_IO_PAUSES_OPS = {{host_shared_KeyIo_Raises_raise}};

/* HOST_SHARED_LONG_IO_PAUSES_OPS is LongIo's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_LONG_IO_PAUSES_OPS = {{host_shared_LongIo_Raises_raise}};

/* HOST_SHARED_FLAG_IO_PAUSES_OPS is FlagIo's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_FLAG_IO_PAUSES_OPS = {{host_shared_FlagIo_Raises_raise}};

/* HOST_SHARED_GUARD_PAUSES_OPS is Guard's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_GUARD_PAUSES_OPS = {{host_shared_Guard_Raises_raise}};

/* HOST_SHARED_LONG_GUARD_PAUSES_OPS is LongGuard's table of Pauses. */
const HostSharedPausesOps HOST_SHARED_LONG_GUARD_PAUSES_OPS = {{host_shared_LongGuard_Raises_raise}};

#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_HOST_TESTING)
/* host_shared_raising returns the capability to raise that a pause's holder carries. */
StdAny
host_shared_raising(HostSharedPauses at)
{
	return at.ops->raises.raise(at.self);
}
#endif

/*
 * host_shared_open opens a published record at a location. It answers
 * nothing for a location that is stale or a record that isn't published,
 * which the crate that keeps the location says the meaning of.
 */
bool
host_shared_open(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
				 bool *has_result, HostSharedHeld *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(within.ops->raises.raise(within.self));
	HostSharedKind kind = host_shared_Kind_pool(s);
	bool		has_place;
	uint64		place;

	if (!host.ops->shared_open(host.self, &raise, &kind, location, &has_place, &place, error))
		return false;
	*has_result = has_place;
	if (has_place)
		*result = (HostSharedHeld)
	{
		host, place
	};
	return true;
}

/* host_shared_Held_record returns the record, reached for as long as the handle lives. */
HostSharedRecord
host_shared_Held_record(const HostSharedHeld *self)
{
	return (HostSharedRecord)
	{
		self->place
	};
}

/*
 * host_shared_Held_place returns the record's place, for a caller that adds
 * to its atomics while the handle lives where it may take no lock and get no
 * result, a commit's critical section.
 */
HostSharedHeldPlace
host_shared_Held_place(const HostSharedHeld *self)
{
	return (HostSharedHeldPlace)
	{
		self->place
	};
}

/* host_shared_HeldPlace_Clone_clone copies the place. */
HostSharedHeldPlace
host_shared_HeldPlace_Clone_clone(const HostSharedHeldPlace *self)
{
	return *self;
}

/* host_shared_HeldPlace_add adds to one of the record's atomics, wrapping, as Record's atomic with an add does. */
void
host_shared_HeldPlace_add(HostSharedHeldPlace self, size_t a, uint64 value)
{
	HostHost	host = host_current();
	HostSharedOp op = {HOST_SHARED_OP_ADD};

	op.u.add = value;
	(void) host.ops->atomic(host.self, self.place, a, op);
}

/*
 * host_shared_HeldPlace_exchange writes one of the record's atomics if it
 * holds a value, as Record's atomic with an exchange does, returning its
 * value before.
 */
uint64
host_shared_HeldPlace_exchange(HostSharedHeldPlace self, size_t a, uint64 old, uint64 new_)
{
	HostHost	host = host_current();
	HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};

	op.u.exchange.f0 = old;
	op.u.exchange.f1 = new_;
	return host.ops->atomic(host.self, self.place, a, op);
}

/* host_shared_PlaceCell_Default_default makes a cell that holds no place. */
void
host_shared_PlaceCell_Default_default(HostSharedPlaceCell *result)
{
	std_AtomicU64_new(&result->place, 0);
}

/* host_shared_PlaceCell_set holds a place, or none. */
void
host_shared_PlaceCell_set(HostSharedPlaceCell *self, bool has_place, HostSharedHeldPlace place)
{
	std_AtomicU64_store_relaxed(&self->place, has_place ? place.place + 1 : 0);
}

/* host_shared_PlaceCell_get returns the place the cell holds, if any. */
HostSharedHeldPlaceOption
host_shared_PlaceCell_get(HostSharedPlaceCell *self)
{
	uint64		stored = std_AtomicU64_load_relaxed(&self->place);
	HostSharedHeldPlaceOption result = {0};

	if (stored != 0)
	{
		result.has = true;
		result.place.place = stored - 1;
	}
	return result;
}

/* host_shared_Held_drop closes the record. */
void
host_shared_Held_drop(HostSharedHeld *self)
{
	self->host.ops->shared_close(self->host.self, self->place);
}

/*
 * host_shared_reclaim looks at a published record at a location for a
 * pool's reclaim, taking it when no process holds it and no other reclaim is
 * looking at it. Its caller shows the entry's Io or the guard it read the
 * location under, of which the reclaim reads nothing.
 */
bool
host_shared_reclaim(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
					HostSharedLook *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_pool(s);
	HostSharedLooked looked;

	if (!host.ops->shared_reclaim(host.self, &kind, location, &looked, error))
		return false;
	switch (looked.tag)
	{
		case HOST_SHARED_LOOKED_MINE:
			*result = (HostSharedLook)
			{
				HOST_SHARED_LOOK_MINE,
				{
					{
						host, looked.u.mine
					}
				}
			};
			break;
		case HOST_SHARED_LOOKED_THEIRS:
			*result = (HostSharedLook)
			{
				HOST_SHARED_LOOK_THEIRS
			};
			break;
		case HOST_SHARED_LOOKED_USED:
			*result = (HostSharedLook)
			{
				HOST_SHARED_LOOK_USED
			};
			break;
	}
	return true;
}

/* host_shared_Reclaim_record returns the record, reached for as long as the reclaim looks at it. */
HostSharedRecord
host_shared_Reclaim_record(const HostSharedReclaim *self)
{
	return (HostSharedRecord)
	{
		self->place
	};
}

/*
 * host_shared_Reclaim_free frees the record if no process opened it since
 * the reclaim took it, and otherwise gives it up, reporting whether it freed
 * it; it takes the reclaim, whose drop follows.
 */
bool
host_shared_Reclaim_free(HostSharedReclaim *self)
{
	bool		freed;
	volatile bool self_live = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		freed = self->host.ops->shared_reclaimed(self->host.self, self->place);
		self_live = false;
		host_shared_Reclaim_drop(self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (self_live)
			host_shared_Reclaim_drop(self);
		std_unwind_resume(&unwind);
	}
	return freed;
}

/* host_shared_Reclaim_drop gives up a record the reclaim still has. */
void
host_shared_Reclaim_drop(HostSharedReclaim *self)
{
	self->host.ops->shared_unreclaim(self->host.self, self->place);
}

/* host_shared_Look_drop is Look's drop glue: a taken record's reclaim's. */
void
host_shared_Look_drop(HostSharedLook *self)
{
	if (self->tag == HOST_SHARED_LOOK_MINE)
		host_shared_Reclaim_drop(&self->u.mine);
}

/*
 * host_shared_sweep lists the published records of a pool: for each, its
 * location and the value of its atomic a, read with no handle. A record is
 * listed only if its slot held it, published, both before and after the
 * read, so the value is that record's. The list is a hint: a record in it
 * may be claimed or freed, and its slot made again, as soon as the sweep has
 * passed it, so a caller acts on a pair only by opening, claiming, or
 * reclaiming its location, and a record published after the sweep passed its
 * slot isn't in it.
 */
bool
host_shared_sweep(HostSharedWithin within, const HostSharedPoolOps *s, size_t a, StdVec *result,
				  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(within.ops->raises.raise(within.self));
	HostSharedKind kind = host_shared_Kind_pool(s);

	return host.ops->shared_sweep(host.self, &raise, &kind, a, result, error);
}

/*
 * host_shared_gone reports whether the record a location named no longer has
 * its slot: the slot's generation has moved on, or the slot is spent. It
 * takes no handle, any process may ask at any time, true is final, and a
 * location outside the shape's pool is refused.
 */
bool
host_shared_gone(const HostSharedPoolOps *s, uint64 location, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_pool(s);

	return host.ops->shared_gone(host.self, &kind, location, result, error);
}

/*
 * host_shared_ready reports whether the server made the fork's shared memory
 * for this process, which a bootstrap process, running no preload window, has
 * none of, so its work over shared records has nothing to do.
 */
bool
host_shared_ready(void)
{
	HostHost	host = host_current();

	return host.ops->shared_ready(host.self);
}

/* host_shared_single returns the server's one record of a shape. */
bool
host_shared_single(const HostSharedSingleOps *s, HostSharedRecord *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_single(s);
	uint64		place;

	if (!host.ops->shared_single(host.self, &kind, &place, error))
		return false;
	*result = (HostSharedRecord)
	{
		place
	};
	return true;
}

/* host_shared_Record_at returns the record at a place, which its caller reached it by. */
HostSharedRecord
host_shared_Record_at(uint64 place)
{
	return (HostSharedRecord)
	{
		place
	};
}

/* host_shared_Record_lock takes a lock under which no file I/O runs, exclusively. */
bool
host_shared_Record_lock(HostSharedRecord self, HostIoTakesShort io, size_t l,
						HostSharedGuard *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;

	io.ops->io_doors.read_doors.sealed.parts(io.self, &held, &raise);
	host = host_current();
	if (!host_shared_take(host, raise, self.place, l, true, error))
		return false;
	*result = host_shared_Guard_new(self.place, l, held, raise);
	return true;
}

/* host_shared_Record_lock_shared takes a lock under which no file I/O runs, shared. */
bool
host_shared_Record_lock_shared(HostSharedRecord self, HostIoTakesShort io, size_t l,
							   HostSharedGuard *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;

	io.ops->io_doors.read_doors.sealed.parts(io.self, &held, &raise);
	host = host_current();
	if (!host_shared_take(host, raise, self.place, l, false, error))
		return false;
	*result = host_shared_Guard_new(self.place, l, held, raise);
	return true;
}

/* host_shared_Record_lock_long takes a lock held across file I/O, exclusively. */
bool
host_shared_Record_lock_long(HostSharedRecord self, HostIoTakesShort io, size_t l,
							 HostSharedLongGuard *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;

	io.ops->io_doors.read_doors.sealed.parts(io.self, &held, &raise);
	host = host_current();
	if (!host_shared_take(host, raise, self.place, l, true, error))
		return false;
	*result = host_shared_LongGuard_new(self.place, l, held, raise);
	return true;
}

/* host_shared_Record_lock_long_shared takes a lock held across file I/O, shared. */
bool
host_shared_Record_lock_long_shared(HostSharedRecord self, HostIoIo *io, size_t l,
									HostSharedLongGuard *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;

	host_io_Io_Sealed_parts(io, &held, &raise);
	host = host_current();
	if (!host_shared_take(host, raise, self.place, l, false, error))
		return false;
	*result = host_shared_LongGuard_new(self.place, l, held, raise);
	return true;
}

/* host_shared_Record_flag sets a flag, waiting while another process holds it, and refusing one its own process holds. */
bool
host_shared_Record_flag(HostSharedRecord self, HostIoTakesShort io, size_t f,
						HostSharedFlagGuard *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;
	HostMayRaise may_raise;
	HostHost	taker;

	io.ops->io_doors.read_doors.sealed.parts(io.self, &held, &raise);
	host = host_current();
	may_raise = host_MayRaise_of(raise);
	if (!host.ops->flag_take(host.self, &may_raise, self.place, f, error))
		return false;
	taker = host_current();
	*result = (HostSharedFlagGuard)
	{
		taker, self.place, f, held, raise
	};
	return true;
}

/* host_shared_Record_atomic operates on an atomic, returning its value before. */
uint64
host_shared_Record_atomic(HostSharedRecord self, size_t a, HostSharedOp op)
{
	HostHost	host = host_current();

	return host.ops->atomic(host.self, self.place, a, op);
}

/*
 * host_shared_Record_atomic_in operates on an atomic of a run by its number
 * in the run, returning its value before, and refuses a number past the run
 * as an internal error.
 */
bool
host_shared_Record_atomic_in(HostSharedRecord self, size_t a_first, size_t a_count, size_t number,
							 HostSharedOp op, uint64 *result, HostError *error)
{
	if (number < a_count)
	{
		HostHost	host = host_current();
		size_t		atomic = STD_ADD(usize, a_first, number, 812, 53);

		*result = host.ops->atomic(host.self, self.place, atomic, op);
		return true;
	}
	else
	{
		static const char text[] = "an atomic of a record's run is past the run";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/* host_shared_Record_names returns one of the record's atomics as a word that takes a page's name at the page's link. */
HostSharedNamed
host_shared_Record_names(HostSharedRecord self, size_t a)
{
	return (HostSharedNamed)
	{
		self, a
	};
}

/*
 * host_shared_Record_names_past returns one of the record's atomics as a word
 * that takes a name at a link one past it, so that zero names nothing, as a
 * slots array's name does.
 */
HostSharedPast
host_shared_Record_names_past(HostSharedRecord self, size_t a)
{
	return (HostSharedPast)
	{
		host_shared_Record_names(self, a)
	};
}

/* host_shared_Named_store writes the atomic, whatever it held; it can't fail. */
void
host_shared_Named_store(const HostSharedNamed *self, uint64 value)
{
	host_shared_stored(self->record.place, self->atomic, value);
}

/* host_shared_Past_store writes one past a name to the atomic, whatever it held; it can't fail. */
void
host_shared_Past_store(const HostSharedPast *self, uint64 name)
{
	host_shared_Named_store(&self->named, STD_ADD(u64, name, 1, 850, 26));
}

/* host_shared_Past_load returns the name the atomic holds one past, or nothing for zero. */
bool
host_shared_Past_load(const HostSharedPast *self, uint64 *result)
{
	HostHost	host = host_current();
	HostSharedOp load = {HOST_SHARED_OP_LOAD};
	uint64		value = host.ops->atomic(host.self, self->named.record.place, self->named.atomic, load);

	return std_checked_sub_u64(value, 1, result);
}

/* host_shared_Past_clear stores zero over the atomic if it holds one past a name, answering whether it did. */
bool
host_shared_Past_clear(const HostSharedPast *self, uint64 name)
{
	HostHost	host = host_current();
	HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};
	uint64		before;

	op.u.exchange.f0 = STD_ADD(u64, name, 1, 860, 74);
	op.u.exchange.f1 = 0;
	before = host.ops->atomic(host.self, self->named.record.place, self->named.atomic, op);
	return before == STD_ADD(u64, name, 1, 860, 91);
}

/* host_shared_stored writes an atomic of the record at a place, whatever it held, by exchanges from the value it last saw. */
void
host_shared_stored(uint64 place, size_t atomic, uint64 value)
{
	HostHost	host = host_current();
	HostSharedOp load = {HOST_SHARED_OP_LOAD};
	uint64		held = host.ops->atomic(host.self, place, atomic, load);

	for (;;)
	{
		HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};
		uint64		before;

		op.u.exchange.f0 = held;
		op.u.exchange.f1 = value;
		before = host.ops->atomic(host.self, place, atomic, op);
		if (before == held)
			return;
		held = before;
	}
}

/* host_shared_Record_word returns a word of the record's run, refusing an index past the run. */
bool
host_shared_Record_word(HostSharedRecord self, const HostSharedSingleOps *s, size_t index,
						HostSharedRunWord *result, HostError *error)
{
	HostHost	host = host_current();
	size_t		atomics = host.ops->shared_atomics(host.self, self.place);
	size_t		run = STD_SUB(usize, atomics, s->shape.atomics, 880, 21);

	if (index >= run)
	{
		static const char text[] = "a word of a single record's run is past the run";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	*result = (HostSharedRunWord)
	{
		self, STD_ADD(usize, s->shape.atomics, index, 883, 44)
	};
	return true;
}

/* host_shared_RunWord_load reads the word. */
uint64
host_shared_RunWord_load(const HostSharedRunWord *self)
{
	HostHost	host = host_current();
	HostSharedOp load = {HOST_SHARED_OP_LOAD};

	return host.ops->atomic(host.self, self->record.place, self->atomic, load);
}

/* host_shared_RunWord_store writes the word, whatever it held. */
void
host_shared_RunWord_store(const HostSharedRunWord *self, uint64 value)
{
	host_shared_stored(self->record.place, self->atomic, value);
}

/* host_shared_RunWord_compare_exchange writes a value if the word holds the one expected, returning the value it held. */
uint64
host_shared_RunWord_compare_exchange(const HostSharedRunWord *self, uint64 expected, uint64 value)
{
	HostHost	host = host_current();
	HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};

	op.u.exchange.f0 = expected;
	op.u.exchange.f1 = value;
	return host.ops->atomic(host.self, self->record.place, self->atomic, op);
}

/* host_shared_RunWord_fetch_add adds to the word, wrapping, and returns the value it held. */
uint64
host_shared_RunWord_fetch_add(const HostSharedRunWord *self, uint64 value)
{
	HostHost	host = host_current();
	HostSharedOp op = {HOST_SHARED_OP_ADD};

	op.u.add = value;
	return host.ops->atomic(host.self, self->record.place, self->atomic, op);
}

/*
 * host_shared_name returns the word a slots array holds for the record at a
 * location it names: one past the location, so that zero names no record.
 */
static uint64
host_shared_name(uint64 location)
{
	return STD_ADD(u64, location, 1, 919, 5);
}

/* host_shared_Record_names_barrier reports whether a slots array names the record a barrier's guard is of. */
static bool
host_shared_Record_names_barrier(HostSharedRecord self, size_t i, const HostSharedLongGuard *barrier)
{
	uint64		named = barrier->host.ops->slots_name(barrier->host.self, self.place, i);
	uint64		location = barrier->host.ops->shared_location(barrier->host.self, barrier->place);
	uint64		mine = host_shared_name(location);

	return named == mine;
}

/* host_shared_Record_mark_door is pause!'s door in mark, which calls host's pause. */
static bool
host_shared_Record_mark_door(HostSharedPauses at, const char *point, HostError *error)
{
	return host_pause(at, point, error);
}

/*
 * host_shared_Record_mark names the record of a barrier, held exclusively,
 * as the one whose barrier guards a slots array, and empties the array,
 * reporting whether it did: it does when the array names no record, or one
 * whose slot was given back since, and changes nothing when the array names a
 * record that still has its slot, or when another mark names its record
 * first. The fork's tests hold it right after its load of the name
 * (012_slots.pl: the mark that lost the name answers false).
 */
bool
host_shared_Record_mark(HostSharedRecord self, size_t i, HostSharedLongGuard *barrier,
						bool *result, HostError *error)
{
	HostHost	host = barrier->host;
	uint64		loaded = host.ops->slots_name(host.self, self.place, i);
	uint64		mine;

	if (!host_shared_Record_mark_door(HOST_SHARED_LONG_GUARD_PAUSES(barrier), "doltgres-shared-marking",
									  error))
		return false;
	if (loaded != 0 && host.ops->shared_live(host.self, STD_SUB(u64, loaded, 1, 943, 44)))
	{
		*result = false;
		return true;
	}
	mine = host_shared_name(host.ops->shared_location(host.self, barrier->place));
	if (!host.ops->slots_rename(host.self, self.place, i, loaded, mine))
	{
		*result = false;
		return true;
	}
	host.ops->slots_empty(host.self, self.place, i);
	*result = true;
	return true;
}

/* host_shared_Record_named returns the location of the record a slots array names, or nothing when it names none. */
bool
host_shared_Record_named(HostSharedRecord self, size_t i, uint64 *result)
{
	HostHost	host = host_current();

	return std_checked_sub_u64(host.ops->slots_name(host.self, self.place, i), 1, result);
}

/*
 * host_shared_Record_unmark clears a slots array's name by one swap from the
 * name of the barrier's record, refusing a barrier of a record the array
 * doesn't name.
 */
bool
host_shared_Record_unmark(HostSharedRecord self, size_t i, HostSharedLongGuard *barrier,
						  HostError *error)
{
	uint64		mine = host_shared_name(barrier->host.ops->shared_location(barrier->host.self,
																		   barrier->place));

	if (!barrier->host.ops->slots_rename(barrier->host.self, self.place, i, mine, 0))
	{
		*error = host_shared_unnamed();
		return false;
	}
	return true;
}

/* host_shared_Record_read returns the claimed slots of a slots array, refusing a barrier of a record it doesn't name. */
bool
host_shared_Record_read(HostSharedRecord self, size_t i, HostSharedLongGuard *barrier,
						StdVec *result, HostError *error)
{
	if (!host_shared_Record_names_barrier(self, i, barrier))
	{
		*error = host_shared_unnamed();
		return false;
	}
	*result = barrier->host.ops->slots_read(barrier->host.self, self.place, i);
	return true;
}

/*
 * host_shared_Record_claim claims a slot of a slots array for an entry's
 * bytes, reporting whether it did: none through a barrier of a record the
 * array doesn't name, and none once every slot is claimed.
 */
bool
host_shared_Record_claim(HostSharedRecord self, size_t i, const HostSharedLongGuard *barrier,
						 const uint8 *bytes, size_t bytes_len, bool *result, HostError *error)
{
	HostMayRaise raise;

	if (!host_shared_Record_names_barrier(self, i, barrier))
	{
		*result = false;
		return true;
	}
	raise = host_MayRaise_of(barrier->raise);
	return barrier->host.ops->slots_claim(barrier->host.self, &raise, self.place, i, bytes, bytes_len,
										  result, error);
}

/* host_shared_unnamed returns the error of a slots array reached through a barrier of a record it doesn't name. */
static HostError
host_shared_unnamed(void)
{
	static const char text[] = "a slots array doesn't name the record of the barrier it is reached through";
	StdString	message;

	std_string_from_Checked(&message, text, sizeof(text) - 1);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* host_shared_take takes a record's lock. */
static bool
host_shared_take(HostHost host, StdAny raise, uint64 place, size_t lock, bool exclusive,
				 HostError *error)
{
	HostMayRaise may_raise = host_MayRaise_of(raise);

	return host.ops->shared_lock(host.self, &may_raise, place, lock, exclusive, error);
}

/* host_shared_Guard_new returns the guard of a lock just taken. */
static HostSharedGuard
host_shared_Guard_new(uint64 place, size_t index, HostIoHeld *held, StdAny raise)
{
	return (HostSharedGuard)
	{
		host_current(), place, index, held, raise
	};
}

/* host_shared_Guard_held lends the taker's lock scope, for backend locks under this one. */
HostIoHeld *
host_shared_Guard_held(HostSharedGuard *self)
{
	return self->held;
}

/* host_shared_Guard_place returns the place of the record whose lock the guard holds. */
uint64
host_shared_Guard_place(const HostSharedGuard *self)
{
	return self->place;
}

/* host_shared_Guard_record returns the record whose lock the guard holds, reached for as long as the guard is. */
HostSharedRecord
host_shared_Guard_record(const HostSharedGuard *self)
{
	return host_shared_Record_at(self->place);
}

/* host_shared_Guard_lock takes a later lock of the same record exclusively. */
bool
host_shared_Guard_lock(HostSharedGuard *self, size_t n, HostSharedGuard *result, HostError *error)
{
	if (!host_shared_take(self->host, self->raise, self->place, n, true, error))
		return false;
	*result = host_shared_Guard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_Guard_lock_shared takes a later lock of the same record shared. */
bool
host_shared_Guard_lock_shared(HostSharedGuard *self, size_t n, HostSharedGuard *result,
							  HostError *error)
{
	if (!host_shared_take(self->host, self->raise, self->place, n, false, error))
		return false;
	*result = host_shared_Guard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_Guard_read_blob returns the bytes of a blob this lock guards. */
StdVec
host_shared_Guard_read_blob(HostSharedGuard *self, size_t i)
{
	return self->host.ops->blob_read(self->host.self, self->place, i);
}

/* host_shared_Guard_entry returns the bytes of an entry of an array this lock guards, refusing an index past the array's count. */
bool
host_shared_Guard_entry(HostSharedGuard *self, size_t i, size_t index, StdVec *result,
						HostError *error)
{
	return self->host.ops->entry_read(self->host.self, self->place, i, index, result, error);
}

/*
 * host_shared_GuardExclusive_write_blob writes the bytes of a blob this lock,
 * held exclusively, guards, in place, refusing bytes longer than the blob's
 * largest size with nothing written.
 */
bool
host_shared_GuardExclusive_write_blob(HostSharedGuard *self, size_t i, const uint8 *bytes, size_t bytes_len,
									  HostError *error)
{
	return self->host.ops->blob_write(self->host.self, self->place, i, bytes, bytes_len, error);
}

/*
 * host_shared_GuardExclusive_set_entry writes the bytes of an entry of an
 * array this lock, held exclusively, guards, in place, refusing an index past
 * the array's count with nothing written.
 */
bool
host_shared_GuardExclusive_set_entry(HostSharedGuard *self, size_t i, size_t index, const uint8 *bytes,
									 size_t bytes_len, HostError *error)
{
	return self->host.ops->entry_write(self->host.self, self->place, i, index, bytes, bytes_len,
									   error);
}

/* host_shared_Guard_drop releases the lock. */
void
host_shared_Guard_drop(HostSharedGuard *self)
{
	self->host.ops->shared_unlock(self->host.self, self->place, self->index);
}

/* host_shared_LongGuard_new returns the guard of a long lock just taken. */
static HostSharedLongGuard
host_shared_LongGuard_new(uint64 place, size_t index, HostIoHeld *held, StdAny raise)
{
	return (HostSharedLongGuard)
	{
		host_current(), place, index, held, raise
	};
}

/* host_shared_LongGuard_io lends the capability for the lock's file I/O and for later locks of the same record. */
HostSharedLongIo
host_shared_LongGuard_io(HostSharedLongGuard *self)
{
	return (HostSharedLongIo)
	{
		self->place, self->held, self->raise
	};
}

/* host_shared_LongGuard_read_blob returns the bytes of a blob this lock guards. */
StdVec
host_shared_LongGuard_read_blob(HostSharedLongGuard *self, size_t i)
{
	return self->host.ops->blob_read(self->host.self, self->place, i);
}

/*
 * host_shared_LongGuard_write_blob writes the bytes of a blob this lock, held
 * exclusively, guards, in place, refusing bytes longer than the blob's
 * largest size with nothing written.
 */
bool
host_shared_LongGuard_write_blob(HostSharedLongGuard *self, size_t i, const uint8 *bytes,
								 size_t bytes_len, HostError *error)
{
	return self->host.ops->blob_write(self->host.self, self->place, i, bytes, bytes_len, error);
}

/* host_shared_LongGuard_drop releases the lock. */
void
host_shared_LongGuard_drop(HostSharedLongGuard *self)
{
	self->host.ops->shared_unlock(self->host.self, self->place, self->index);
}

/* host_shared_LongIo_lock takes a later lock of the same record exclusively. */
bool
host_shared_LongIo_lock(HostSharedLongIo *self, size_t n, HostSharedGuard *result, HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, n, true, error))
		return false;
	*result = host_shared_Guard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_LongIo_lock_shared takes a later lock of the same record shared. */
bool
host_shared_LongIo_lock_shared(HostSharedLongIo *self, size_t n, HostSharedGuard *result,
							   HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, n, false, error))
		return false;
	*result = host_shared_Guard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_LongIo_lock_long takes a later lock of the same record that is held across file I/O, exclusively. */
bool
host_shared_LongIo_lock_long(HostSharedLongIo *self, size_t n, HostSharedLongGuard *result,
							 HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, n, true, error))
		return false;
	*result = host_shared_LongGuard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_LongIo_lock_long_shared takes a later lock of the same record that is held across file I/O, shared. */
bool
host_shared_LongIo_lock_long_shared(HostSharedLongIo *self, size_t n, HostSharedLongGuard *result,
									HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, n, false, error))
		return false;
	*result = host_shared_LongGuard_new(self->place, n, self->held, self->raise);
	return true;
}

/* host_shared_LongIo_Deref_deref is the capability's lock scope. */
static const void *
host_shared_LongIo_Deref_deref(const void *self)
{
	return ((const HostSharedLongIo *) self)->held;
}

/* host_shared_LongIo_DerefMut_deref_mut is the capability's lock scope. */
static void *
host_shared_LongIo_DerefMut_deref_mut(void *self)
{
	return ((HostSharedLongIo *) self)->held;
}

/* host_shared_LongIo_Sealed_parts returns the capability's lock scope and the capability of pg's it borrows. */
static void
host_shared_LongIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostSharedLongIo *io = (HostSharedLongIo *) self;

	*out_0 = io->held;
	*out_1 = io->raise;
}

/* HOST_SHARED_LONG_IO_IO_DOORS_OPS is a long lock's capability's tables of Sealed, ReadDoors, and IoDoors. */
const HostIoIoDoorsOps HOST_SHARED_LONG_IO_IO_DOORS_OPS = {
	.read_doors = {
		.sealed = {host_shared_LongIo_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_shared_LongIo_Deref_deref}, host_shared_LongIo_DerefMut_deref_mut},
	},
};

/* host_shared_FlagGuard_io lends the capability for the flag's operation. */
HostSharedFlagIo
host_shared_FlagGuard_io(HostSharedFlagGuard *self)
{
	return (HostSharedFlagIo)
	{
		self->place, self->held, self->raise
	};
}

/* host_shared_FlagGuard_drop clears the flag and wakes its waiters. */
void
host_shared_FlagGuard_drop(HostSharedFlagGuard *self)
{
	self->host.ops->flag_clear(self->host.self, self->place, self->index);
}

/* host_shared_FlagIo_check fails with Postgres' error once an interrupt is pending. */
bool
host_shared_FlagIo_check(HostSharedFlagIo *self, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(self->raise);

	return host.ops->check_interrupts(host.self, &raise, error);
}

/* host_shared_FlagIo_flag sets a later flag of the same record. */
bool
host_shared_FlagIo_flag(HostSharedFlagIo *self, size_t g, HostSharedFlagGuard *result,
						HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(self->raise);

	HostHost	taker;

	if (!host.ops->flag_take(host.self, &raise, self->place, g, error))
		return false;
	taker = host_current();
	*result = (HostSharedFlagGuard)
	{
		taker, self->place, g, self->held, self->raise
	};
	return true;
}

/* host_shared_FlagIo_lock takes a lock of the same record under which no file I/O runs, exclusively. */
bool
host_shared_FlagIo_lock(HostSharedFlagIo *self, size_t l, HostSharedGuard *result, HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, l, true, error))
		return false;
	*result = host_shared_Guard_new(self->place, l, self->held, self->raise);
	return true;
}

/* host_shared_FlagIo_lock_shared takes a lock of the same record under which no file I/O runs, shared. */
bool
host_shared_FlagIo_lock_shared(HostSharedFlagIo *self, size_t l, HostSharedGuard *result,
							   HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, l, false, error))
		return false;
	*result = host_shared_Guard_new(self->place, l, self->held, self->raise);
	return true;
}

/* host_shared_FlagIo_lock_long takes a lock of the same record held across file I/O. */
bool
host_shared_FlagIo_lock_long(HostSharedFlagIo *self, size_t l, HostSharedLongGuard *result,
							 HostError *error)
{
	HostHost	host = host_current();

	if (!host_shared_take(host, self->raise, self->place, l, true, error))
		return false;
	*result = host_shared_LongGuard_new(self->place, l, self->held, self->raise);
	return true;
}

/* host_shared_FlagIo_Deref_deref is the capability's lock scope. */
const void *
host_shared_FlagIo_Deref_deref(const void *self)
{
	return ((const HostSharedFlagIo *) self)->held;
}

/* host_shared_FlagIo_DerefMut_deref_mut is the capability's lock scope. */
void *
host_shared_FlagIo_DerefMut_deref_mut(void *self)
{
	return ((HostSharedFlagIo *) self)->held;
}

/* host_shared_FlagIo_Sealed_parts returns the capability's lock scope and the capability of pg's it borrows. */
void
host_shared_FlagIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostSharedFlagIo *io = (HostSharedFlagIo *) self;

	*out_0 = io->held;
	*out_1 = io->raise;
}

/* HOST_SHARED_FLAG_IO_IO_DOORS_OPS is a flag's capability's tables of Sealed, ReadDoors, and IoDoors. */
const HostIoIoDoorsOps HOST_SHARED_FLAG_IO_IO_DOORS_OPS = {
	.read_doors = {
		.sealed = {host_shared_FlagIo_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_shared_FlagIo_Deref_deref}, host_shared_FlagIo_DerefMut_deref_mut},
	},
};

/* host_shared_epochs takes the epoch lock exclusively, to queue what a reader may still see or to compare the readers' epochs. */
bool
host_shared_epochs(HostIoIo *io, HostSharedEpochs *result, HostError *error)
{
	HostIoHeld *held;
	StdAny		raise;
	HostHost	host;
	HostMayRaise may_raise;

	host_io_Io_Sealed_parts(io, &held, &raise);
	host = host_current();
	may_raise = host_MayRaise_of(raise);
	if (!host.ops->epochs_lock(host.self, &may_raise, error))
		return false;
	*result = (HostSharedEpochs)
	{
		host_current()
	};
	return true;
}

/*
 * host_shared_Epochs_advance returns the current epoch, at which what is
 * queued now waits, and advances the current epoch past it.
 */
uint64
host_shared_Epochs_advance(HostSharedEpochs *self)
{
	return self->host.ops->epochs_advance(self->host.self);
}

/*
 * host_shared_Epochs_oldest returns the oldest epoch a live reader started
 * at, or the current epoch when no reader is live.
 */
uint64
host_shared_Epochs_oldest(HostSharedEpochs *self)
{
	return self->host.ops->epochs_oldest(self->host.self);
}

/* host_shared_Epochs_drop releases the epoch lock. */
void
host_shared_Epochs_drop(HostSharedEpochs *self)
{
	self->host.ops->epochs_unlock(self->host.self);
}

/* host_shared_read starts a reader at the current epoch, published before the reader reads what the epochs keep. */
bool
host_shared_read(HostIoIo *io, HostSharedReader *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	uint64		epoch;

	if (!host.ops->epochs_read(host.self, &raise, &epoch, error))
		return false;
	*result = (HostSharedReader)
	{
		host, epoch
	};
	return true;
}

/* host_shared_Reader_epoch returns the epoch the reader started at. */
uint64
host_shared_Reader_epoch(const HostSharedReader *self)
{
	return self->epoch;
}

/* host_shared_Reader_drop ends the reader. */
void
host_shared_Reader_drop(HostSharedReader *self)
{
	self->host.ops->epochs_end(self->host.self, self->epoch);
}

/* host_shared_proc_number returns the process's number among the server's processes, which shared entries it owns name. */
uint32
host_shared_proc_number(void)
{
	HostHost	host = host_current();

	return host.ops->proc_number(host.self);
}

/*
 * host_shared_mine returns the running process's handle of its own slot of
 * words. Its caller shows the entry's Io or a registration's view of it, of
 * which it reads nothing.
 */
bool
host_shared_mine(HostIoTakesShort io, const HostSharedProcSlotsOps *p, HostSharedMine *result,
				 HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);
	uint64		place;

	if (!host.ops->words_mine(host.self, &kind, &place, error))
		return false;
	*result = (HostSharedMine)
	{
		host, place
	};
	return true;
}

/* host_shared_Mine_store stores one of the process's words. */
void
host_shared_Mine_store(const HostSharedMine *self, size_t w, uint64 value)
{
	self->host.ops->word_store(self->host.self, self->place, w, value);
}

/* host_shared_Mine_load loads one of the process's words. */
uint64
host_shared_Mine_load(const HostSharedMine *self, size_t w)
{
	HostSharedOp load = {HOST_SHARED_OP_LOAD};

	return self->host.ops->atomic(self->host.self, self->place, w, load);
}

/* host_shared_Mine_store_at stores the process's word at a place. */
void
host_shared_Mine_store_at(const HostSharedMine *self, HostSharedAt at, uint64 value)
{
	self->host.ops->word_store(self->host.self, self->place, at.index, value);
}

/* host_shared_Mine_load_at loads the process's word at a place. */
uint64
host_shared_Mine_load_at(const HostSharedMine *self, HostSharedAt at)
{
	HostSharedOp load = {HOST_SHARED_OP_LOAD};

	return self->host.ops->atomic(self->host.self, self->place, at.index, load);
}

/* host_shared_Mine_exchange stores one of the process's words over the value given, returning the value it found. */
uint64
host_shared_Mine_exchange(const HostSharedMine *self, size_t w, uint64 loaded, uint64 value)
{
	HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};

	op.u.exchange.f0 = loaded;
	op.u.exchange.f1 = value;
	return self->host.ops->atomic(self->host.self, self->place, w, op);
}

/* host_shared_Mine_id returns a number naming the process among the slots' holders, for a word another process stores. */
uint64
host_shared_Mine_id(const HostSharedMine *self)
{
	return self->place;
}

/*
 * host_shared_store_own stores one of the running process's words of a listed
 * kind with no result, for a store inside a critical section: the slot's
 * place is the kind's position and the process's number.
 */
void
host_shared_store_own(const HostSharedProcSlotsOps *p, const HostSharedListed *listed, size_t w,
					  uint64 value)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);

	host.ops->word_store_own(host.self, listed->index, &kind, w, value);
}

/*
 * host_shared_processes returns the numbers of the processes whose slot can
 * hold a word, each of which load takes. A process that takes its slot after
 * the list was made isn't in it.
 */
bool
host_shared_processes(const HostSharedProcSlotsOps *p, StdVec *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);

	return host.ops->words_processes(host.self, &kind, result, error);
}

/* host_shared_load loads one word of a process's slot, refusing a process number past the server's processes. */
bool
host_shared_load(const HostSharedProcSlotsOps *p, size_t w, uint32 proc, uint64 *result,
				 HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);

	return host.ops->word_load(host.self, &kind, proc, w, result, error);
}

/* host_shared_load_at loads the word at a place of a process's slot, refusing a process number past the server's processes. */
bool
host_shared_load_at(const HostSharedProcSlotsOps *p, uint32 proc, HostSharedAt at, uint64 *result,
					HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);

	return host.ops->word_load(host.self, &kind, proc, at.index, result, error);
}

/* host_shared_theirs finds a process's slot of a kind of words, refusing a process with none. */
bool
host_shared_theirs(const HostSharedProcSlotsOps *p, uint32 proc, HostSharedTheirs *result, HostError *error)
{
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(p);
	uint64		place;

	if (!host.ops->words_place(host.self, &kind, proc, &place, error))
		return false;
	*result = (HostSharedTheirs)
	{
		host, place
	};
	return true;
}

/* host_shared_Theirs_exchange stores one of the process's words over the value given, returning the value it found. */
uint64
host_shared_Theirs_exchange(const HostSharedTheirs *self, size_t w, uint64 loaded, uint64 value)
{
	HostSharedOp op = {HOST_SHARED_OP_EXCHANGE};

	op.u.exchange.f0 = loaded;
	op.u.exchange.f1 = value;
	return self->host.ops->atomic(self->host.self, self->place, w, op);
}

#ifdef DOLTGRES_TEST_HOST

/* host_shared_tests_line returns a table's line of a kind of an id, laid out as no shape needs. */
static HostSharedKind
host_shared_tests_line(uint64 id, HostSharedForm form)
{
	return (HostSharedKind)
	{
		.id = id, .blobs = STD_DANGLING, .arrays = STD_DANGLING, .form = form
	};
}

/* host_shared_tests_aged_closure_0 is aged's |_| 1, its kind's count. */
static size_t
host_shared_tests_aged_closure_0(const HostSharedLimits *limits)
{
	return 1;
}

/* host_shared_tests_aged returns the line of an aged kind of an id whose records name the kinds of the ids given. */
static HostSharedKind
host_shared_tests_aged(uint64 id, const uint64 *names, size_t names_len)
{
	HostSharedForm form = {HOST_SHARED_FORM_AGED};

	form.u.aged.count = host_shared_tests_aged_closure_0;
	form.u.aged.names = names;
	form.u.aged.names_len = names_len;
	return host_shared_tests_line(id, form);
}

/* test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_0 is the single line's |_| 0. */
static size_t
test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_0(const HostSharedLimits *limits)
{
	return 0;
}

/* test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_1 is the table's single line's |_| 0. */
static size_t
test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_1(const HostSharedLimits *limits)
{
	return 0;
}

/*
 * test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names: a
 * table lists an aged kind before each aged kind it names, and names only
 * aged kinds it lists.
 */
void
test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names(void)
{
	static const uint64 two[] = {2};
	static const uint64 four[] = {4};
	static const uint64 three[] = {3};
	static const uint64 one[] = {1};
	HostSharedForm single_form = {HOST_SHARED_FORM_SINGLE};
	HostSharedKind single;

	single_form.u.single.run = test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_0;
	single = host_shared_tests_line(3, single_form);
	{
		HostSharedForm form = {HOST_SHARED_FORM_SINGLE};
		HostSharedKind kinds[3];

		form.u.single.run = test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names_closure_1;
		kinds[0] = host_shared_tests_aged(1, two, 1);
		kinds[1] = host_shared_tests_aged(2, STD_DANGLING, 0);
		kinds[2] = host_shared_tests_line(3, form);
		STD_ASSERT(1439, 9, host_shared_ordered(kinds, 3),
				   "ordered(&[aged(1, &[2]), aged(2, &[]), line(3, Form::Single { run: |_| 0 })])");
	}
	{
		HostSharedKind kinds[2];

		kinds[0] = host_shared_tests_aged(2, STD_DANGLING, 0);
		kinds[1] = host_shared_tests_aged(1, two, 1);

		STD_ASSERT(1440, 9, !host_shared_ordered(kinds, 2), "!ordered(&[aged(2, &[]), aged(1, &[2])])");
	}
	{
		HostSharedKind kinds[2];

		kinds[0] = host_shared_tests_aged(1, four, 1);
		kinds[1] = host_shared_tests_aged(2, STD_DANGLING, 0);

		STD_ASSERT(1441, 9, !host_shared_ordered(kinds, 2), "!ordered(&[aged(1, &[4]), aged(2, &[])])");
	}
	{
		HostSharedKind kinds[2];

		kinds[0] = host_shared_tests_aged(1, three, 1);
		kinds[1] = single;

		STD_ASSERT(1442, 9, !host_shared_ordered(kinds, 2), "!ordered(&[aged(1, &[3]), single])");
	}
	{
		HostSharedKind kinds[1];

		kinds[0] = host_shared_tests_aged(1, one, 1);

		STD_ASSERT(1443, 9, !host_shared_ordered(kinds, 1), "!ordered(&[aged(1, &[1])])");
	}
}

#endif							/* DOLTGRES_TEST_HOST */
