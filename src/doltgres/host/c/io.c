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
 * The capability to do file I/O and to raise, which an entry gets once and
 * which every storage lock borrows while it is held, so that no file door
 * runs under one (L1). Only pghost makes one; a frontend program's build has
 * only a ReadIo, so no write door can be called there.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"

#ifdef DOLTGRES_TEST_HOST
/* host_io_Held_testing returns a lock scope for this crate's tests of the locks, which reach no seam. */
HostIoHeld
host_io_Held_testing(void)
{
	return (HostIoHeld)
	{
		0
	};
}
#endif

/* host_io_InDrop_new returns the lock scope of a drop. */
HostIoInDrop
host_io_InDrop_new(void)
{
	return (HostIoInDrop)
	{
		0
	};
}

#ifndef FRONTEND
/* host_io_Io_new_for_pghost makes the capability around pg's, for pghost alone. */
HostIoIo
host_io_Io_new_for_pghost(StdAny raise)
{
	return (HostIoIo)
	{
		.held =
		{
			0
		}, .raise = raise
	};
}
#endif

/*
 * host_io_Io_check fails with Postgres' error once an interrupt is pending,
 * such as a cancel of the statement, the check each item a long loop hands
 * out makes (L2).
 */
bool
host_io_Io_check(HostIoIo *self, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(self->raise);

	return host.ops->check_interrupts(host.self, &raise, error);
}

/*
 * host_io_Io_hold holds the process's interrupts, as Postgres'
 * HOLD_INTERRUPTS does, until a resume, so a cancel or a termination waits for
 * work that must finish once begun (bugs row 461).
 */
void
host_io_Io_hold(HostIoIo *self)
{
	HostHost	host = host_current();

	(void) self;
	host.ops->hold_interrupts(host.self);
}

/* host_io_Io_resume gives back a hold of the process's interrupts, as Postgres' RESUME_INTERRUPTS does. */
void
host_io_Io_resume(HostIoIo *self)
{
	HostHost	host = host_current();

	(void) self;
	host.ops->resume_interrupts(host.self);
}

#ifdef DOLTGRES_HOST_TESTING
/*
 * host_io_Io_testing returns a test's capability, around nothing, which only
 * the simulation's host accepts, since pghost opens only the capability of
 * pg's.
 */
HostIoIo
host_io_Io_testing(void)
{
	return (HostIoIo)
	{
		.held =
		{
			0
		}, .raise =
		{
			&STD_UNIT_TYPE_ID, STD_DANGLING
		}
	};
}
#endif

/* host_io_Io_Deref_deref is an Io's Held. */
const void *
host_io_Io_Deref_deref(const void *self)
{
	return &((const HostIoIo *) self)->held;
}

/* host_io_Io_DerefMut_deref_mut is an Io's Held, to lend. */
void *
host_io_Io_DerefMut_deref_mut(void *self)
{
	return &((HostIoIo *) self)->held;
}

#ifndef FRONTEND
/* host_io_Waits_new_for_pghost makes the capability, for pghost alone. */
HostIoWaits
host_io_Waits_new_for_pghost(void)
{
	return (HostIoWaits)
	{
		0
	};
}
#endif

#ifdef DOLTGRES_HOST_TESTING
/* host_io_Waits_testing returns a test's capability, which only the simulation's host serves. */
HostIoWaits
host_io_Waits_testing(void)
{
	return (HostIoWaits)
	{
		0
	};
}
#endif

/* host_io_Waits_striping lends the token a key lock takes, borrowed from the capability, which no wait door takes. */
HostIoStriping
host_io_Waits_striping(HostIoWaits *self)
{
	return (HostIoStriping)
	{
		0
	};
}

/* host_io_sealed_Sealed_pushed is Sealed's default pushed, which keeps nothing. */
void
host_io_sealed_Sealed_pushed(void *self)
{
}

/* host_io_Io_Sealed_parts returns an Io's lock scope and capability. */
void
host_io_Io_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostIoIo   *io = (HostIoIo *) self;

	*out_0 = &io->held;
	*out_1 = io->raise;
}

/*
 * HOST_IO_IO_TAKES_SHORT_OPS is an Io's tables of Sealed, ReadDoors, IoDoors,
 * and TakesShort.
 */
const HostIoTakesShortOps HOST_IO_IO_TAKES_SHORT_OPS = {
	.io_doors = {
		.read_doors = {
			.sealed = {host_io_Io_Sealed_parts, host_io_sealed_Sealed_pushed},
			.deref_mut = {{host_io_Io_Deref_deref}, host_io_Io_DerefMut_deref_mut},
		},
	},
};

/* host_io_ShortIo_new returns the view a short lender's guard lends. */
HostIoShortIo
host_io_ShortIo_new(HostIoTakesShort io)
{
	HostIoHeld *held;
	StdAny		raise;

	io.ops->io_doors.read_doors.sealed.parts(io.self, &held, &raise);
	return (HostIoShortIo)
	{
		.held = held, .raise = raise
	};
}

/* host_io_ShortIo_Deref_deref is a ShortIo's Held. */
const void *
host_io_ShortIo_Deref_deref(const void *self)
{
	return ((const HostIoShortIo *) self)->held;
}

/* host_io_ShortIo_DerefMut_deref_mut is a ShortIo's Held, to lend. */
void *
host_io_ShortIo_DerefMut_deref_mut(void *self)
{
	return ((HostIoShortIo *) self)->held;
}

/* host_io_ShortIo_Sealed_parts returns a ShortIo's lock scope and capability. */
void
host_io_ShortIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostIoShortIo *io = (HostIoShortIo *) self;

	*out_0 = io->held;
	*out_1 = io->raise;
}

/*
 * HOST_IO_SHORT_IO_IO_DOORS_OPS is a ShortIo's tables of Sealed, ReadDoors,
 * and IoDoors.
 */
const HostIoIoDoorsOps HOST_IO_SHORT_IO_IO_DOORS_OPS = {
	.read_doors = {
		.sealed = {host_io_ShortIo_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_io_ShortIo_Deref_deref}, host_io_ShortIo_DerefMut_deref_mut},
	},
};

/* host_io_KeyIo_new returns the view a key lock's guard lends. */
HostIoKeyIo
host_io_KeyIo_new(HostIoHeld *held, StdAny raise, bool *pushed)
{
	return (HostIoKeyIo)
	{
		.held = held, .raise = raise, .pushed = pushed
	};
}

/* host_io_KeyIo_Deref_deref is a KeyIo's Held. */
const void *
host_io_KeyIo_Deref_deref(const void *self)
{
	return ((const HostIoKeyIo *) self)->held;
}

/* host_io_KeyIo_DerefMut_deref_mut is a KeyIo's Held, to lend. */
void *
host_io_KeyIo_DerefMut_deref_mut(void *self)
{
	return ((HostIoKeyIo *) self)->held;
}

/* host_io_KeyIo_Sealed_parts returns a KeyIo's lock scope and capability. */
void
host_io_KeyIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostIoKeyIo *io = (HostIoKeyIo *) self;

	*out_0 = io->held;
	*out_1 = io->raise;
}

/* host_io_KeyIo_Sealed_pushed marks a push made under the guard's stripes. */
void
host_io_KeyIo_Sealed_pushed(void *self)
{
	*((HostIoKeyIo *) self)->pushed = true;
}

/*
 * HOST_IO_KEY_IO_TAKES_SHORT_OPS is a KeyIo's tables of Sealed, ReadDoors,
 * IoDoors, and TakesShort.
 */
const HostIoTakesShortOps HOST_IO_KEY_IO_TAKES_SHORT_OPS = {
	.io_doors = {
		.read_doors = {
			.sealed = {host_io_KeyIo_Sealed_parts, host_io_KeyIo_Sealed_pushed},
			.deref_mut = {{host_io_KeyIo_Deref_deref}, host_io_KeyIo_DerefMut_deref_mut},
		},
	},
};

/* host_io_Registering_new returns the view of an entry's Io that a registration's scope lends. */
HostIoRegistering
host_io_Registering_new(HostIoIo *io)
{
	HostIoHeld *held;
	StdAny		raise;

	host_io_Io_Sealed_parts(io, &held, &raise);
	return (HostIoRegistering)
	{
		.held = held, .raise = raise
	};
}

/* host_io_Registering_Deref_deref is a Registering's Held. */
const void *
host_io_Registering_Deref_deref(const void *self)
{
	return ((const HostIoRegistering *) self)->held;
}

/* host_io_Registering_DerefMut_deref_mut is a Registering's Held, to lend. */
void *
host_io_Registering_DerefMut_deref_mut(void *self)
{
	return ((HostIoRegistering *) self)->held;
}

/*
 * host_io_Registering_Sealed_parts returns a Registering's lock scope and
 * capability.
 */
void
host_io_Registering_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostIoRegistering *io = (HostIoRegistering *) self;

	*out_0 = io->held;
	*out_1 = io->raise;
}

/*
 * HOST_IO_REGISTERING_TAKES_SHORT_OPS is a Registering's tables of Sealed,
 * ReadDoors, IoDoors, and TakesShort.
 */
const HostIoTakesShortOps HOST_IO_REGISTERING_TAKES_SHORT_OPS = {
	.io_doors = {
		.read_doors = {
			.sealed = {host_io_Registering_Sealed_parts, host_io_sealed_Sealed_pushed},
			.deref_mut = {{host_io_Registering_Deref_deref}, host_io_Registering_DerefMut_deref_mut},
		},
	},
};

#ifdef FRONTEND
/* host_io_ReadIo_new_for_fehost makes the capability, for fehost alone. */
HostIoReadIo
host_io_ReadIo_new_for_fehost(void)
{
	return (HostIoReadIo)
	{
		.held =
		{
			0
		}, .raise =
		{
			&STD_UNIT_TYPE_ID, STD_DANGLING
		}
	};
}

/* host_io_ReadIo_Deref_deref is a ReadIo's Held. */
const void *
host_io_ReadIo_Deref_deref(const void *self)
{
	return &((const HostIoReadIo *) self)->held;
}

/* host_io_ReadIo_DerefMut_deref_mut is a ReadIo's Held, to lend. */
void *
host_io_ReadIo_DerefMut_deref_mut(void *self)
{
	return &((HostIoReadIo *) self)->held;
}

/* host_io_ReadIo_Sealed_parts returns a ReadIo's lock scope and capability. */
void
host_io_ReadIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1)
{
	HostIoReadIo *io = (HostIoReadIo *) self;

	*out_0 = &io->held;
	*out_1 = io->raise;
}

/* HOST_IO_READ_IO_READ_DOORS_OPS is a ReadIo's tables of Sealed and ReadDoors. */
const HostIoReadDoorsOps HOST_IO_READ_IO_READ_DOORS_OPS = {
	.sealed = {host_io_ReadIo_Sealed_parts, host_io_sealed_Sealed_pushed},
	.deref_mut = {{host_io_ReadIo_Deref_deref}, host_io_ReadIo_DerefMut_deref_mut},
};
#endif
