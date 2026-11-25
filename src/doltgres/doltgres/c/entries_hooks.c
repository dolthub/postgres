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
 * The doltgres crate's entries at core Postgres' catalog call sites
 * (entries.rs' doltgres_storage_unlinked to doltgres_relation_toast_am): an
 * unlinked storage's free, the identity pairs of relations and schemas, the
 * row layouts ALTER TABLE and a column's drop change, the relation being
 * created, and a fork table's TOAST relation.
 */
#ifndef FRONTEND
#include "postgres.h"
#include "catalog/pg_namespace_d.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/entries_tail.h"
#include "doltgres/doltgres/identity.h"
#include "doltgres/doltgres/layouts.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/host/lib.h"
#include "doltgres/pg/lib.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/unwind.h"
#include "doltgres/store/databases.h"
#include "doltgres/txn/state.h"
#include "pg_calls.h"

/*
 * doltgres_entries_storage_unlinked frees the fork's storage of a relfilenode
 * Postgres unlinks, at commit, at abort, and in redo (PLAN item 5's
 * relfilenodes and their files), so the delta versions of a dropped,
 * truncated, or rewritten storage, which no fold of a gone relation takes,
 * leave the budget; a replayed one names the storage as redo's records do
 * (bugs row 443). The transaction's outcome is decided, so a failure is
 * reported at WARNING, as smgr_unlink reports its own, and nothing raises.
 */
bool
doltgres_entries_doltgres_storage_unlinked(const PgCaps *caps, const PgFileLocator *locator, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	DgStorage	located = pg_calls_filelocator_storage(locator);
	HostStorage storage = {.tablespace = located.tablespace, .database = located.database,
	.relation = located.relation};
	HostIoIo	io = pghost_io(raise);
	HostError	failed;
	PgPgError	raised;
	uint32		timeline;

	store_databases_replayed_storage(&storage, pg_calls_replay_read(&timeline));

	if (txn_state_unlinked(&io, &storage, &failed))
		return true;
	raised = pghost_raised(&failed);
	return pg_warn_error(raise, &raised, "freeing the storage of an unlinked relation", error);
}

/* DoltgresEntriesOpenedStep is a step an entry runs with the backend's repository and a reader of the shared state open. */
typedef bool (*DoltgresEntriesOpenedStep) (const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
										   const HostSharedReader *reading, const void *captures, PgPgError *error);

/* doltgres_entries_opened_frame_t is opened's state across its pad: what it opened. */
typedef struct doltgres_entries_opened_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	DoltgresEntriesOpenedStep step;
	const void *captures;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_opened_frame_t;

/* doltgres_entries_opened_pad frees what opened holds. */
static void
doltgres_entries_opened_pad(void *frame_)
{
	doltgres_entries_opened_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_opened_body opens the repository and a reader, then runs the step. */
static void
doltgres_entries_opened_body(void *frame_)
{
	doltgres_entries_opened_frame_t *frame = frame_;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	frame->ok = frame->step(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
							&frame->opened.reading, frame->captures, frame->error);
}

/*
 * doltgres_entries_opened runs a step with the catalog's io, the backend's
 * repository, and a reader of the shared state, as an entry's repository and
 * read open them, releasing the reader and then the repository.
 */
static bool
doltgres_entries_opened(const PgCatalogCaps *caps, DoltgresEntriesOpenedStep step, const void *captures,
						PgPgError *error)
{
	doltgres_entries_opened_frame_t frame = {.caps = caps, .step = step, .captures = captures, .error = error};

	std_landing(&frame, doltgres_entries_opened_body, doltgres_entries_opened_pad);
	doltgres_entries_opened_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_relation_dropped_step is relation_dropped's step: the relation's identity pairs removed. */
static bool
doltgres_entries_relation_dropped_step(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									   const HostSharedReader *reading, const void *captures, PgPgError *error)
{
	return doltgres_identity_dropped(caps, io, repo, reading, captures, error);
}

/*
 * doltgres_entries_doltgres_relation_dropped removes a dropped relation's
 * identity pairs in the dropping transaction, in heap_drop_with_catalog and
 * index_drop (design item 10).
 */
bool
doltgres_entries_doltgres_relation_dropped(const PgCatalogCaps *caps, const PgRel *relation, PgPgError *error)
{
	return !doltgres_identity_users(relation) ||
		doltgres_entries_opened(caps, doltgres_entries_relation_dropped_step, relation, error);
}

/*
 * doltgres_entries_doltgres_altering keeps the row layouts of a relation the
 * fork stores and of its inheritors before an ALTER TABLE subcommand runs, at
 * the start of ATExecCmd (design item 5), with the table's rewrite flags so
 * far.
 */
bool
doltgres_entries_doltgres_altering(const PgCatalogCaps *caps, const PgRel *relation, const PgAlterCmd *cmd,
								   int32 rewrite, PgPgError *error)
{
	return cmd == NULL ||
		doltgres_layouts_altering(caps, relation, pg_calls_alter_subtype(cmd), rewrite, error);
}

/* doltgres_entries_altered_captures_t is altered's step's captures: the relation and the subcommand's type. */
typedef struct doltgres_entries_altered_captures_t
{
	const PgRel *relation;
	AlterTableType subtype;
} doltgres_entries_altered_captures_t;

/* doltgres_entries_altered_step is altered's step: what the subcommand changed of the relation's identity pairs recorded. */
static bool
doltgres_entries_altered_step(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							  const HostSharedReader *reading, const void *captures, PgPgError *error)
{
	const doltgres_entries_altered_captures_t *c = captures;

	return doltgres_identity_altered(caps, io, repo, reading, c->relation, c->subtype, error);
}

/*
 * doltgres_entries_doltgres_altered records what an executed ALTER TABLE
 * subcommand changes of a relation the fork stores and of its inheritors, at
 * the end of ATExecCmd (design items 5 and 10).
 */
bool
doltgres_entries_doltgres_altered(const PgCatalogCaps *caps, const PgRel *relation, const PgAlterCmd *cmd,
								  PgPgError *error)
{
	doltgres_entries_altered_captures_t c = {.relation = relation};

	if (!doltgres_layouts_altered(caps, relation, error))
		return false;
	if (cmd == NULL || !doltgres_identity_users(relation))
		return true;
	c.subtype = pg_calls_alter_subtype(cmd);
	return doltgres_entries_opened(caps, doltgres_entries_altered_step, &c, error);
}

/*
 * doltgres_entries_doltgres_column_dropped chains the transform of a
 * column's drop from a table the fork stores, in doDeletion right after
 * RemoveAttributeById, for every dropped column, ALTER TABLE's and a
 * dependency's cascade's alike (design item 5); a whole relation's drop,
 * attribute number 0, changes no layout.
 */
bool
doltgres_entries_doltgres_column_dropped(const PgCatalogCaps *caps, uint32 relid, int32 attnum, PgPgError *error)
{
	return attnum == 0 || doltgres_layouts_dropped(caps, relid, attnum, error);
}

/* doltgres_entries_object_dropped_step is object_dropped's step: the schema's identity pair removed. */
static bool
doltgres_entries_object_dropped_step(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
									 const HostSharedReader *reading, const void *captures, PgPgError *error)
{
	return doltgres_identity_schema_dropped(caps, io, repo, reading, *(const uint32 *) captures, error);
}

/*
 * doltgres_entries_doltgres_object_dropped removes a dropped schema's
 * identity pair in the dropping transaction, in doDeletion (design item 10),
 * which calls it for an object of any class.
 */
bool
doltgres_entries_doltgres_object_dropped(const PgCatalogCaps *caps, uint32 class_id, uint32 object_id,
										 PgPgError *error)
{
	return class_id != NamespaceRelationId ||
		doltgres_entries_opened(caps, doltgres_entries_object_dropped_step, &object_id, error);
}

/* doltgres_entries_namespace_created_captures_t is namespace_created's step's captures: the schema and its name. */
typedef struct doltgres_entries_namespace_created_captures_t
{
	uint32		namespace;
	const char *name;
} doltgres_entries_namespace_created_captures_t;

/* doltgres_entries_namespace_created_step is namespace_created's step: the schema's identity pair recorded by its name. */
static bool
doltgres_entries_namespace_created_step(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
										const HostSharedReader *reading, const void *captures, PgPgError *error)
{
	const doltgres_entries_namespace_created_captures_t *c = captures;

	return doltgres_identity_schema_created(caps, io, repo, reading, c->namespace, (const uint8 *) c->name,
											strlen(c->name), error);
}

/*
 * doltgres_entries_doltgres_namespace_created records a new schema's identity
 * pair in its creating transaction, in NamespaceCreate after its catalog row
 * is inserted (design item 10, CommPF's ruling (a)), by the name it was made
 * with; a backend's temporary schema, which belongs to no branch, gets none.
 */
bool
doltgres_entries_doltgres_namespace_created(const PgCatalogCaps *caps, uint32 nspoid, const char *nspname, bool istemp,
											PgPgError *error)
{
	doltgres_entries_namespace_created_captures_t c = {.namespace = nspoid, .name = nspname};

	return istemp || doltgres_entries_opened(caps, doltgres_entries_namespace_created_step, &c, error);
}

/*
 * doltgres_entries_doltgres_relation_creating records the relation Postgres
 * is about to create and the relation it is made to rewrite, which
 * doltgres_table_am_routine answers by until the relation's relcache entry
 * holds it.
 */
bool
doltgres_entries_doltgres_relation_creating(const PgCaps *caps, uint32 relid, uint32 relrewrite, PgPgError *error)
{
	doltgres_relation_relation_creating(relid, relrewrite);
	return true;
}

/*
 * doltgres_entries_doltgres_relation_needs_toast_table answers whether a
 * relation needs a TOAST relation as heap's routine does, so a fork table has
 * the TOAST relation Postgres gives it (design item 5).
 */
bool
doltgres_entries_doltgres_relation_needs_toast_table(const PgCaps *caps, const PgRel *rel, bool *result,
													 PgPgError *error)
{
	*result = pg_calls_heap_needs_toast_table(rel);
	return true;
}

/*
 * doltgres_entries_doltgres_relation_toast_am names the access method of a
 * relation's TOAST relation as heap's routine does (design item 5); the fork
 * stores that TOAST relation whatever it names.
 */
bool
doltgres_entries_doltgres_relation_toast_am(const PgCaps *caps, const PgRel *rel, uint32 *result, PgPgError *error)
{
	*result = pg_calls_heap_toast_am(rel);
	return true;
}
#endif							/* FRONTEND */
