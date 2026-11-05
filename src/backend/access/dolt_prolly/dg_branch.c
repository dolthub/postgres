#include "dg.h"
#include "catalog/catalogview.h"
#include "utils/inval.h"
#include "access/doltcatalog.h"
#include "access/doltbranch.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "catalog/pg_dolt_ref.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/utility.h"
#include "access/heapam.h"
#include "access/htup_details.h"

static char *selected_branch;
static const char *execution_branch;

const char *
dg_execution_branch(void)
{
	return execution_branch;
}

const char *
dg_switch_execution_branch(const char *branch)
{
	const char *previous = execution_branch;

	execution_branch = branch;
	return previous;
}

static char *pending_branch;
static bool registered;

static bool session_branch_locked;

static void
branch_session_tag(LOCKTAG *tag, const char *name)
{
	PLHash		hash;
	PLSlice		bytes = {(const uint8 *) name, strlen(name)};

	if (!pl_hash_bytes(bytes, &hash))
		elog(ERROR, "branch hash failed");
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, pl_read32(hash.bytes), pl_read32(hash.bytes + 4), 3);
}

void
dg_branch_require_unused(const char *name)
{
	LOCKTAG		tag;

	branch_session_tag(&tag, name);
	if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("branch %s is in use by another session", name), errhint("Use -f to force deletion.")));
}

const char *
dg_current_branch(void)
{
	if (execution_branch)
		return execution_branch;
	if (IsUnderPostmaster && !session_branch_locked)
	{
		LOCKTAG		tag;

		branch_session_tag(&tag, selected_branch ? selected_branch : "main");
		LockAcquire(&tag, ShareLock, true, false);
		session_branch_locked = true;
	}
	return selected_branch ? selected_branch : "main";
}

/* Backend-local exact branch identities for cached plans; never persisted. */
uint64
dg_branch_cache_id(void)
{
	static List *names;
	const char *branch = dg_current_branch();
	uint64		id = 1;
	ListCell   *lc;
	MemoryContext previous;

	foreach(lc, names)
	{
		if (!strcmp(lfirst(lc), branch))
			return id;
		id++;
	}
	previous = MemoryContextSwitchTo(TopMemoryContext);
	names = lappend(names, pstrdup(branch));
	MemoryContextSwitchTo(previous);
	return id;
}

/*
 * The branch whose roots |rel| reads and writes: a private storage descriptor's
 * own branch, a branch head relation's branch, or else the session's.
 */
static HTAB *durable_branches;

typedef struct DurableBranch
{
	Oid			oid;
	char		branch[NAMEDATALEN * 2];
} DurableBranch;

static void
durable_branches_reset(Datum arg, Oid relid)
{
	if (durable_branches && (!OidIsValid(relid) || CatalogViewIsDurable(relid)))
	{
		hash_destroy(durable_branches);
		durable_branches = NULL;
	}
}

const char *
dg_relation_branch(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	DurableBranch *entry;
	bool		found;

	if (dg_storage_bound(rel))
		return dg_storage_branch(rel);
	if (!CatalogViewIsDurable(relid))
		return dg_current_branch();
	if (!durable_branches)
	{
		HASHCTL		ctl = {0};
		static bool registered = false;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DurableBranch);
		ctl.hcxt = TopMemoryContext;
		durable_branches = hash_create("durable relation branches", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		if (!registered)
		{
			CacheRegisterRelcacheCallback(durable_branches_reset, (Datum) 0);
			registered = true;
		}
	}
	entry = hash_search(durable_branches, &relid, HASH_ENTER, &found);
	if (!found)
	{
		char	   *branch,
				   *name;
		char		kind;

		if (!dg_identity_describe(relid, &branch, &kind, &name))
		{
			hash_search(durable_branches, &relid, HASH_REMOVE, NULL);
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("relation %u no longer belongs to a branch", relid)));
		}
		strlcpy(entry->branch, branch, sizeof(entry->branch));
	}
	return entry->branch;
}

static void
branch_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && pending_branch)
	{
		if (session_branch_locked)
		{
			LOCKTAG		tag;

			branch_session_tag(&tag, selected_branch ? selected_branch : "main");
			LockRelease(&tag, ShareLock, true);
		}
		session_branch_locked = true;
		if (selected_branch)
			pfree(selected_branch);
		selected_branch = pending_branch;
		pending_branch = NULL;
	}
	else if (event == XACT_EVENT_ABORT && pending_branch)
	{
		LOCKTAG		tag;

		branch_session_tag(&tag, pending_branch);
		LockRelease(&tag, ShareLock, true);
		pfree(pending_branch);
		pending_branch = NULL;
	}
}
void
dg_select_branch(const char *name)
{
	if (!registered)
	{
		RegisterXactCallback(branch_xact, NULL);
		registered = true;
	}
	if (pending_branch)
	{
		LOCKTAG		tag;

		branch_session_tag(&tag, pending_branch);
		LockRelease(&tag, ShareLock, true);
		pfree(pending_branch);
	}
	{
		LOCKTAG		tag;

		branch_session_tag(&tag, name);
		LockAcquire(&tag, ShareLock, true, false);
	}
	pending_branch = MemoryContextStrdup(TopMemoryContext, name);
}

/* A branch without local root records starts from its retained history state.
 * Existing relation-local records override this base under the reader snapshot. */
void
dg_branch_base(Relation rel, DGState *state)
{
	MemoryContext caller = CurrentMemoryContext;
	Oid			types[] = {TEXTOID};
	Datum		args[] = {CStringGetTextDatum(psprintf("heads/%s", dg_relation_branch(rel)))};
	bytea	   *root = NULL;
	bytea	   *address = NULL;
	bool		isnull;
	char	   *name = dg_qualified_name(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));
	DGRootEntry entry;

	if (!strcmp(dg_relation_branch(rel), "main"))
		return;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "cannot connect branch SPI");
	MemoryContextSwitchTo(caller);
	SPI_execute_with_args("SELECT c.objchildren[1] FROM pg_catalog.pg_dolt_ref r JOIN pg_catalog.pg_dolt_object_visible c ON c.objhash=r.reftarget WHERE r.refname=$1", 1, types, args, NULL, true, 1);
	MemoryContextSwitchTo(caller);
	if (SPI_processed)
		root = DatumGetByteaPCopy(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();
	if (root && (dg_root_find(root, rel->rd_rel->relkind == RELKIND_INDEX ? "index" : "table", name, &entry) ||
				 dg_root_find(root, "table", name, &entry)) && entry.has_content)
	{
		address = palloc(VARHDRSZ + PL_HASH_SIZE);
		SET_VARSIZE(address, VARHDRSZ + PL_HASH_SIZE);
		memcpy(VARDATA(address), entry.content.bytes, PL_HASH_SIZE);
	}
	if (!address)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation %s has no state on branch %s", RelationGetRelationName(rel), dg_relation_branch(rel))));
	dg_set_base(state, dg_history_open(state->store, address));
}

const char *
dg_quote_identifier(const char *ident)
{
	bool		saved = quote_all_identifiers;
	const char *result;

	quote_all_identifiers = false;
	result = quote_identifier(ident);
	quote_all_identifiers = saved;
	return result;
}

char *
dg_qualified_name(const char *qualifier, const char *ident)
{
	bool		saved = quote_all_identifiers;
	char	   *result;

	quote_all_identifiers = false;
	result = quote_qualified_identifier(qualifier, ident);
	quote_all_identifiers = saved;
	return result;
}
