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
 * The capability to do file I/O and to raise (host's io module): an entry's
 * Io, which every storage lock borrows while it is held, and the views of it
 * a lock's guard or a registration lends.
 */
#ifndef DOLTGRES_HOST_IO_H
#define DOLTGRES_HOST_IO_H

#include "doltgres/std/any.h"

struct HostError;

/*
 * HostIoHeld is the part of an Io a storage lock's guard lends on: it takes
 * further storage locks, and reaches no file door. Only an Io has one, so a
 * drop, which has none, can't take a lock through it.
 */
typedef struct HostIoHeld
{
	char		unused;
} HostIoHeld;

/*
 * HostIoInDrop is what a drop takes a lock under which no file I/O runs with:
 * a take that finds the lock held by a frame the drop unwinds through, or
 * poisoned, gives nothing, and the drop leaves its work to the state's
 * rebuild, so that a drop never panics during an unwind and never rebuilds
 * (H2).
 */
typedef struct HostIoInDrop
{
	char		unused;
} HostIoInDrop;

/*
 * HostIoIo is an entry's capability to do file I/O and to raise, which every
 * file door takes. It is neither cloned nor sent, and is made only by pghost
 * for an entry, around the capability of pg's the entry got. Only the whole
 * Io hands out what reaches a catalog or user code, never a view of it, so a
 * door that reaches either takes the Io itself.
 */
typedef struct HostIoIo
{
	/* held is the lock scope the guards of storage locks borrow. */
	HostIoHeld	held;
	/* raise is the capability of pg's the entry got, which only pghost opens. */
	StdAny		raise;
} HostIoIo;

/*
 * HostIoWaits is an entry's capability to wait on another backend: on its
 * transaction, on a heavyweight lock, on a condition variable, or for the
 * delta's budget, which every such door takes. It is neither cloned nor sent,
 * and is made only by pghost for an entry, as Io is, so a key lock, which
 * borrows it while its stripes are held, keeps every wait from compiling
 * under a stripe ((4b)'s key lock).
 */
typedef struct HostIoWaits
{
	char		unused;
} HostIoWaits;

/*
 * HostIoStriping is the token a key lock takes and no wait door does: a
 * prepare lends it to its window's work, so nothing there can wait while it
 * holds the prepare's reservations ((4b)'s P1).
 */
typedef struct HostIoStriping
{
	char		unused;
} HostIoStriping;

/* HostIoSealedSealedOps is Sealed's table, which only this module's views implement. */
typedef struct HostIoSealedSealedOps
{
	/*
	 * parts returns the view's lock scope and the capability of pg's it
	 * borrows.
	 */
	void		(*parts) (void *self, HostIoHeld **out_0, StdAny *out_1);
	/* pushed marks a push the view made, which only a key lock's view keeps. */
	void		(*pushed) (void *self);
} HostIoSealedSealedOps;

/* HostIoReadDoorsOps is ReadDoors' table: what the doors that only read take. */
typedef struct HostIoReadDoorsOps
{
	HostIoSealedSealedOps sealed;
	StdDerefMutOps deref_mut;
} HostIoReadDoorsOps;

/* HostIoReadDoors is a view that implements ReadDoors. */
typedef struct HostIoReadDoors
{
	const HostIoReadDoorsOps *ops;
	void	   *self;
} HostIoReadDoors;

/* HostIoIoDoorsOps is IoDoors' table: what the file doors that write take. */
typedef struct HostIoIoDoorsOps
{
	HostIoReadDoorsOps read_doors;
} HostIoIoDoorsOps;

/* HostIoIoDoors is a view that implements IoDoors. */
typedef struct HostIoIoDoors
{
	const HostIoIoDoorsOps *ops;
	void	   *self;
} HostIoIoDoors;

/* HostIoTakesShortOps is TakesShort's table: what takes a lender. */
typedef struct HostIoTakesShortOps
{
	HostIoIoDoorsOps io_doors;
} HostIoTakesShortOps;

/* HostIoTakesShort is a view that implements TakesShort. */
typedef struct HostIoTakesShort
{
	const HostIoTakesShortOps *ops;
	void	   *self;
} HostIoTakesShort;

/*
 * HostIoShortIo is the view of an Io that a short lender's guard lends for
 * its one bounded I/O step: it does file I/O and takes locks under which no
 * I/O runs, but takes no lender and checks no interrupt.
 */
typedef struct HostIoShortIo
{
	/* held is the lock scope of the Io it views. */
	HostIoHeld *held;
	/* raise is the capability of pg's the Io borrows. */
	StdAny		raise;
} HostIoShortIo;

/*
 * HostIoKeyIo is the view of an Io that a key lock's guard lends while its
 * stripes are held ((4b)'s key lock): it does file I/O, reads transactions'
 * status, writes the WAL, opens the checkpoint window, pauses, and takes the
 * memtable's locks, and reaches no door that takes the whole Io, so no user
 * code runs under a stripe, and no door that waits, which take the entry's
 * Waits. Nothing turns it back into an Io.
 */
typedef struct HostIoKeyIo
{
	/* held is the lock scope of the Io it views. */
	HostIoHeld *held;
	/* raise is the capability of pg's the Io borrows. */
	StdAny		raise;
	/* pushed is the guard's mark of a push made under its stripes. */
	bool	   *pushed;
} HostIoKeyIo;

/*
 * HostIoRegistering is the view of an Io that a root's registration lends for
 * as long as it lasts (design item 6): it does file I/O, takes locks and
 * short lenders, and reaches no door that takes the whole Io, so no user code
 * runs while a registration lasts. Nothing turns it back into an Io.
 */
typedef struct HostIoRegistering
{
	/* held is the lock scope of the Io it views. */
	HostIoHeld *held;
	/* raise is the capability of pg's the Io borrows. */
	StdAny		raise;
} HostIoRegistering;

#ifdef FRONTEND
/*
 * HostIoReadIo is a frontend program's capability to read files, which the
 * read doors take and the doors that write don't, made only by fehost, the
 * frontend program's host.
 */
typedef struct HostIoReadIo
{
	/* held is the lock scope the guards of storage locks borrow. */
	HostIoHeld	held;

	/*
	 * raise is the capability a door hands the host, which fehost never
	 * opens.
	 */
	StdAny		raise;
} HostIoReadIo;
#endif

#ifdef DOLTGRES_TEST_HOST
extern HostIoHeld host_io_Held_testing(void);
#endif
extern HostIoInDrop host_io_InDrop_new(void);
#ifndef FRONTEND
extern HostIoIo host_io_Io_new_for_pghost(StdAny raise);
#endif
extern bool host_io_Io_check(HostIoIo *self, struct HostError *error);
#ifdef DOLTGRES_HOST_TESTING
extern HostIoIo host_io_Io_testing(void);
#endif
extern const void *host_io_Io_Deref_deref(const void *self);
extern void *host_io_Io_DerefMut_deref_mut(void *self);
#ifndef FRONTEND
extern HostIoWaits host_io_Waits_new_for_pghost(void);
#endif
#ifdef DOLTGRES_HOST_TESTING
extern HostIoWaits host_io_Waits_testing(void);
#endif
extern HostIoStriping host_io_Waits_striping(HostIoWaits *self);
extern void host_io_sealed_Sealed_pushed(void *self);
extern void host_io_Io_Sealed_parts(void *self, HostIoHeld **out_0,
									StdAny *out_1);
extern HostIoShortIo host_io_ShortIo_new(HostIoTakesShort io);
extern const void *host_io_ShortIo_Deref_deref(const void *self);
extern void *host_io_ShortIo_DerefMut_deref_mut(void *self);
extern void host_io_ShortIo_Sealed_parts(void *self, HostIoHeld **out_0,
										 StdAny *out_1);
extern HostIoKeyIo host_io_KeyIo_new(HostIoHeld *held, StdAny raise,
									 bool *pushed);
extern const void *host_io_KeyIo_Deref_deref(const void *self);
extern void *host_io_KeyIo_DerefMut_deref_mut(void *self);
extern void host_io_KeyIo_Sealed_parts(void *self, HostIoHeld **out_0,
									   StdAny *out_1);
extern void host_io_KeyIo_Sealed_pushed(void *self);
extern HostIoRegistering host_io_Registering_new(HostIoIo *io);
extern const void *host_io_Registering_Deref_deref(const void *self);
extern void *host_io_Registering_DerefMut_deref_mut(void *self);
extern void host_io_Registering_Sealed_parts(void *self, HostIoHeld **out_0,
											 StdAny *out_1);
#ifdef FRONTEND
extern HostIoReadIo host_io_ReadIo_new_for_fehost(void);
extern const void *host_io_ReadIo_Deref_deref(const void *self);
extern void *host_io_ReadIo_DerefMut_deref_mut(void *self);
extern void host_io_ReadIo_Sealed_parts(void *self, HostIoHeld **out_0,
										StdAny *out_1);
#endif

/* Each view's tables, the one of each trait it implements. */
extern const HostIoTakesShortOps HOST_IO_IO_TAKES_SHORT_OPS;
extern const HostIoIoDoorsOps HOST_IO_SHORT_IO_IO_DOORS_OPS;
extern const HostIoTakesShortOps HOST_IO_KEY_IO_TAKES_SHORT_OPS;
extern const HostIoTakesShortOps HOST_IO_REGISTERING_TAKES_SHORT_OPS;
#ifdef FRONTEND
extern const HostIoReadDoorsOps HOST_IO_READ_IO_READ_DOORS_OPS;
#endif

/*
 * The coercions of a view to a trait it implements, which Rust makes
 * implicitly where a generic door takes it.
 */
#define HOST_IO_IO_READ_DOORS(io) \
	((HostIoReadDoors) {&HOST_IO_IO_TAKES_SHORT_OPS.io_doors.read_doors, (io)})
#define HOST_IO_IO_IO_DOORS(io) \
	((HostIoIoDoors) {&HOST_IO_IO_TAKES_SHORT_OPS.io_doors, (io)})
#define HOST_IO_IO_TAKES_SHORT(io) \
	((HostIoTakesShort) {&HOST_IO_IO_TAKES_SHORT_OPS, (io)})
#define HOST_IO_TAKES_SHORT_IO_DOORS(t) \
	((HostIoIoDoors) {&(t).ops->io_doors, (t).self})
#define HOST_IO_IO_DOORS_READ_DOORS(t) \
	((HostIoReadDoors) {&(t).ops->read_doors, (t).self})

#endif							/* DOLTGRES_HOST_IO_H */
