/* Immutable revision-bound catalog views shared by native and SQL lookups. */
#ifndef CATALOGVIEW_H
#define CATALOGVIEW_H
#include "access/htup.h"
#include "fmgr.h"
#include "access/skey.h"
#include "access/sdir.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 * Durable OIDs name prolly objects on branch heads (see pg_dolt_identity.h).
 * They are shared by all backends and replicated, and the ordinary OID counter
 * never assigns them. Backend-local handles name objects in read-only
 * revisions and never enter WAL or canonical schema bytes.
 */
#define FirstDurableObjectId ((Oid) 0xA0000000U)
#define FirstCatalogViewObjectId ((Oid) 0xC0000000U)
#define CatalogViewIsDurable(oid) ((oid) >= FirstDurableObjectId && (oid) < FirstCatalogViewObjectId)
#define CatalogViewIsLeased(oid) ((oid) >= FirstCatalogViewObjectId)

/* View rows have TIDs from a block range no physical catalog reaches. */
#define CatalogViewFirstBlock ((BlockNumber) 0xF0000000U)
/* Rows views share have TIDs from a range of their own. */
#define CatalogViewBaseBlock ((BlockNumber) 0xF8000000U)
struct catcache;
typedef struct CatalogView CatalogView;
typedef struct CatalogViewBase CatalogViewBase;
typedef struct CatalogViewScan CatalogViewScan;

extern CatalogView *CatalogViewCreate(const char *identity);

/* Immutable rows shared by views, such as the bootstrap catalogs'. */
extern CatalogViewBase *CatalogViewBaseCreate(void);
extern void CatalogViewBaseAddRows(CatalogViewBase *base, Oid catalog, List *tuples);
extern void CatalogViewBaseRelease(CatalogViewBase *base);
extern void CatalogViewSetBase(CatalogView *view, CatalogViewBase *base);
typedef bool (*CatalogViewBaseFilter) (Oid catalog, HeapTuple tuple, void *arg);
extern void CatalogViewSetBaseFilter(CatalogView *view, CatalogViewBaseFilter filter, void *arg);
extern MemoryContext CatalogViewMemory(CatalogView *view);
extern void CatalogViewAddRows(CatalogView *view, Oid catalog, List *tuples);
extern void CatalogViewSeal(CatalogView *view);
extern void CatalogViewSetPartial(CatalogView *view, bool partial);
extern bool CatalogViewIsPartial(CatalogView *view);
extern int	catalog_view_cache_entries;
extern void CatalogViewSetLoader(CatalogView *view, void (*loader) (CatalogView *, void *), void *arg);
extern void CatalogViewEnsureLoaded(CatalogView *view);
extern void CatalogViewSetCreator(CatalogView *view, TransactionId creator);
extern bool CatalogViewCreatorAborted(CatalogView *view);
extern void CatalogViewRetain(CatalogView *view);
extern uint64 CatalogViewReferences(CatalogView *view);
extern void CatalogViewRelease(CatalogView *view);
extern CatalogView *CatalogViewCurrent(void);
extern Oid	CatalogViewNamespace(CatalogView *view, Oid namespaceid);
extern uint64 CatalogViewCacheId(CatalogView *view);
extern Oid	CatalogViewBindObject(CatalogView *view, Oid objectclass, const char *identity, Oid storage);
extern void CatalogViewBindDurable(CatalogView *view, Oid objectclass, const char *identity, Oid oid, Oid storage);
extern void CatalogViewBindShared(CatalogView *view, Oid objectclass, const char *identity, Oid oid);
extern bool CatalogViewBinds(CatalogView *view, Oid oid);
extern void CatalogViewForgetDurable(void);
extern Oid	CatalogViewFindObject(CatalogView *view, Oid objectclass, const char *identity);
extern Oid	CatalogViewLookupObject(CatalogView *view, Oid objectclass, const char *identity);
extern CatalogView *CatalogViewObjectContext(Oid handle);
extern void CatalogViewMarkBranchHead(CatalogView *view);
extern bool CatalogViewIsObjectIdType(Oid type);
extern CatalogView *CatalogViewCallContext(FunctionCallInfo fcinfo);
extern HeapTuple CatalogViewFunctionTuple(Oid functionid);
extern Oid	CatalogViewObjectStorage(Oid handle);
extern void CatalogViewObjectSetRetained(CatalogView *view, Oid handle, bytea *schema, bytea *content);
extern bytea *CatalogViewObjectSchema(Oid handle);
extern bytea *CatalogViewObjectContent(Oid handle);
extern const char *CatalogViewObjectIdentity(Oid handle, Oid objectclass);
extern CatalogView *CatalogViewSelectKeys(CatalogView *fallback, TupleDesc desc, int nkeys, const int *attributes, const Datum *values);

/* Callers must restore the previous value in PG_FINALLY, including on ERROR. */
extern CatalogView *CatalogViewSwitch(CatalogView *view);
extern bool CatalogViewContains(CatalogView *view, Oid catalog);
extern struct catcache *CatalogViewCache(CatalogView *view, struct catcache *base);
extern void CatalogViewInvalidateCache(int cacheid, uint32 hashvalue);
extern void CatalogViewRejectPhysicalAccess(Relation relation);
extern void CatalogViewRejectBranchWrite(Relation relation, ItemPointer tid, HeapTuple tuple);

/* Private command-visible catalog mutation state for native DDL. */
extern bool CatalogViewCaptureUpdate(Relation relation, ItemPointer tid, HeapTuple tuple);
extern bool CatalogViewCaptureInsert(Relation relation, HeapTuple tuple);
extern bool CatalogViewCaptureDelete(Relation relation, ItemPointer tid);
typedef enum CatalogViewStorageEffect
{
	CATALOG_STORAGE_CREATE,
	CATALOG_STORAGE_DROP,
	CATALOG_STORAGE_BUILD
} CatalogViewStorageEffect;
extern void CatalogViewSetStorageHandler(CatalogView *view, void (*handler) (Relation, CatalogViewStorageEffect));
extern bool CatalogViewCaptureStorage(Relation relation, CatalogViewStorageEffect effect);
extern void CatalogViewBeginMutation(CatalogView *view);
extern void CatalogViewSetMutationValidator(CatalogView *view, void (*validator) (Relation, HeapTuple));
extern bool CatalogViewIsProvisional(CatalogView *view);
extern Oid	CatalogViewNewObject(Oid catalog);
extern void CatalogViewNameObject(Oid handle, const char *identity);
extern void CatalogViewMutationCommandEnd(void);
extern List *CatalogViewTuples(CatalogView *view, Oid catalog);
extern List *CatalogViewTuplesWith(CatalogView *view, Oid catalog, AttrNumber attribute, Oid value);
extern void CatalogViewForEachChange(CatalogView *view, void (*callback) (Oid catalog, HeapTuple tuple, void *arg), void *arg);
extern void CatalogViewRetireMutation(CatalogView *view);
extern CatalogViewScan *CatalogViewBeginScan(Relation relation, int nkeys, ScanKey keys);
extern CatalogViewScan *CatalogViewBeginScanSnapshot(Relation relation, int nkeys, ScanKey keys, Snapshot snapshot);
extern void CatalogViewOrderScan(CatalogViewScan *scan, Relation index);
extern HeapTuple CatalogViewNext(CatalogViewScan *scan, ScanDirection direction);
extern void CatalogViewRescan(CatalogViewScan *scan, ScanKey keys);
extern void CatalogViewEndScan(CatalogViewScan *scan);
#endif
